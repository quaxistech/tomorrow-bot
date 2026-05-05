/// @file order_watchdog.cpp
/// @brief Реализация Order Watchdog — монитора жизненного цикла ордеров

#include "pipeline/order_watchdog.hpp"
#include <algorithm>
#include <time.h>

namespace tb::pipeline {

// ============================================================
// Конструктор
// ============================================================

OrderWatchdog::OrderWatchdog(
    OrderWatchdogConfig config,
    std::shared_ptr<execution::ExecutionEngine> exec_engine,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(config)
    , exec_engine_(std::move(exec_engine))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        stale_orders_cancelled_  = metrics_->counter(
            "pipeline_watchdog_stale_cancelled",    {{"symbol", "all"}});
        unknown_recovery_detected_ = metrics_->counter(
            "pipeline_watchdog_unknown_recovery",   {{"symbol", "all"}});
        partial_fill_timeout_    = metrics_->counter(
            "pipeline_watchdog_partial_fill_timeout", {{"symbol", "all"}});
    }
}

// ============================================================
// run_check
// ============================================================

std::vector<WatchdogReport> OrderWatchdog::run_check() {
    std::vector<WatchdogReport> reports;

    if (!exec_engine_) return reports;

    // BUG-S4-24: guard against overflow — cap check_interval_ms at 60 000 ms (60s)
    if (config_.check_interval_ms > 60'000) {
        logger_->error("watchdog", "check_interval_ms exceeds 60000ms, capping to 60000ms",
            {{"configured_ms", std::to_string(config_.check_interval_ms)}});
        config_.check_interval_ms = 60'000;
    }

    // Проверяем интервал между запусками
    const int64_t now_ns = clock_->now().get();
    const int64_t interval_ns = config_.check_interval_ms * 1'000'000LL;
    if (last_check_ns_ > 0 && (now_ns - last_check_ns_) < interval_ns) {
        return reports;
    }
    last_check_ns_ = now_ns;

    // Получить все активные ордера из Execution Engine
    auto active = exec_engine_->active_orders();

    // BUG-S4-14: removed duplicate exec_engine_->cancel_timed_out_orders() call here.
    // The watchdog loop below already classifies and cancels timed-out orders
    // (Open/CancelPending with age > max_open_order_ms → WatchdogOrderAction::Cancel),
    // so calling cancel_timed_out_orders() here as well caused double cancellation.

    for (const auto& order : active) {
        WatchdogOrderAction action = classify_order(order, now_ns);
        if (action == WatchdogOrderAction::Ok) continue;

        // BUG-S34-03: negative age_ms when NTP backward jump → no timeout ever fires.
        int64_t age_ms = std::max(int64_t{0}, now_ns - order.last_updated.get()) / 1'000'000LL;

        WatchdogReport report;
        report.order_id = order.order_id;
        report.action   = action;
        report.age_ms   = age_ms;

        switch (action) {
            case WatchdogOrderAction::Cancel:
                report.reason = "timeout: state=" + execution::to_string(order.state)
                    + " age=" + std::to_string(age_ms) + "ms";

                // Отменяем ордер на бирже
                exec_engine_->cancel(order.order_id);

                if (cancel_cb_) {
                    cancel_cb_(order.order_id, report.reason);
                }

                if (order.state == execution::OrderState::PartiallyFilled && partial_fill_timeout_) {
                    partial_fill_timeout_->increment();
                } else if (stale_orders_cancelled_) {
                    stale_orders_cancelled_->increment();
                }

                logger_->warn("watchdog", "Ордер принудительно отменён watchdog-ом",
                    {{"order_id", order.order_id.get()},
                     {"state", execution::to_string(order.state)},
                     {"age_ms", std::to_string(age_ms)}});
                break;

            case WatchdogOrderAction::RecoverState:
                report.reason = "UnknownRecovery: age=" + std::to_string(age_ms) + "ms";

                if (unknown_recovery_detected_) {
                    unknown_recovery_detected_->increment();
                }

                logger_->error("watchdog", "Ордер в состоянии UnknownRecovery",
                    {{"order_id", order.order_id.get()},
                     {"age_ms", std::to_string(age_ms)}});

                if (alert_cb_) {
                    alert_cb_(report);
                }
                break;

            case WatchdogOrderAction::ForceClose:
                report.reason = "force_close: critical partial fill age=" + std::to_string(age_ms) + "ms";

                exec_engine_->cancel(order.order_id);

                if (cancel_cb_) {
                    cancel_cb_(order.order_id, report.reason);
                }

                if (stale_orders_cancelled_) {
                    stale_orders_cancelled_->increment();
                }

                logger_->warn("watchdog", "Ордер принудительно закрыт watchdog-ом",
                    {{"order_id", order.order_id.get()},
                     {"age_ms", std::to_string(age_ms)}});
                break;

            default:
                break;
        }

        reports.push_back(std::move(report));
    }

    return reports;
}

// ============================================================
// classify_order
// ============================================================

WatchdogOrderAction OrderWatchdog::classify_order(
    const execution::OrderRecord& order, int64_t now_ns) const
{
    if (order.last_updated.get() <= 0) return WatchdogOrderAction::Ok;

    // BUG-S34-03: clamp to 0 to avoid negative age when NTP backward jump
    const int64_t age_ms = std::max(int64_t{0}, now_ns - order.last_updated.get()) / 1'000'000LL;

    switch (order.state) {
        case execution::OrderState::PendingAck:
            if (age_ms > config_.max_pending_ack_ms) {
                return WatchdogOrderAction::Cancel;
            }
            break;

        case execution::OrderState::UnknownRecovery:
            if (age_ms > config_.max_unknown_recovery_ms) {
                return WatchdogOrderAction::RecoverState;
            }
            break;

        case execution::OrderState::PartiallyFilled:
            if (age_ms > config_.max_partial_fill_ms) {
                // Критически зависший частично исполненный ордер
                return WatchdogOrderAction::ForceClose;
            }
            break;

        case execution::OrderState::Open:
        case execution::OrderState::CancelPending:
            if (age_ms > config_.max_open_order_ms) {
                return WatchdogOrderAction::Cancel;
            }
            break;

        default:
            break;
    }

    return WatchdogOrderAction::Ok;
}

} // namespace tb::pipeline
