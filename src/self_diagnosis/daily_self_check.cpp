/**
 * @file daily_self_check.cpp
 * @brief Реализация ежедневной самопроверки
 */

#include "self_diagnosis/daily_self_check.hpp"

#include <chrono>

namespace tb::self_diagnosis {

DailySelfCheck::DailySelfCheck(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        gauge_self_check_status_ = metrics_->gauge("tb_self_check_status", {});
        counter_self_check_runs_ = metrics_->counter("tb_self_check_runs_total", {});
    }
}

void DailySelfCheck::register_check(
    std::string name, CheckSeverity severity, CheckFn fn)
{
    checks_.push_back({std::move(name), severity, std::move(fn)});
}

void DailySelfCheck::register_standard_checks(
    CheckFn exchange_connectivity,
    CheckFn margin_balance,
    CheckFn position_reconciliation,
    CheckFn market_data_feed,
    CheckFn system_clock_sync)
{
    register_check("exchange_connectivity", CheckSeverity::Critical,
                   std::move(exchange_connectivity));
    register_check("margin_balance", CheckSeverity::Critical,
                   std::move(margin_balance));
    register_check("position_reconciliation", CheckSeverity::Critical,
                   std::move(position_reconciliation));
    register_check("market_data_feed", CheckSeverity::Warning,
                   std::move(market_data_feed));
    register_check("system_clock_sync", CheckSeverity::Warning,
                   std::move(system_clock_sync));
}

SelfCheckResult DailySelfCheck::run() {
    SelfCheckResult result;
    result.performed_at = clock_ ? clock_->now() : Timestamp(0);

    auto wall_start = std::chrono::steady_clock::now();

    logger_->info("DailySelfCheck", "Starting daily self-check", {
        {"checks_registered", std::to_string(checks_.size())}
    });

    bool all_critical = true;
    bool all_passed = true;

    for (const auto& reg : checks_) {
        CheckItem item;
        item.name = reg.name;
        item.severity = reg.severity;

        auto check_start = std::chrono::steady_clock::now();
        try {
            auto [ok, detail] = reg.fn();
            item.passed = ok;
            item.detail = std::move(detail);
        } catch (const std::exception& ex) {
            item.passed = false;
            item.detail = std::string("Exception: ") + ex.what();
        }
        auto check_end = std::chrono::steady_clock::now();
        item.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            check_end - check_start).count();

        if (!item.passed) {
            all_passed = false;
            if (item.severity == CheckSeverity::Critical) {
                all_critical = false;
                logger_->critical("DailySelfCheck", "CRITICAL check FAILED: " + item.name, {
                    {"detail", item.detail},
                    {"duration_ms", std::to_string(item.duration_ms)},
                    {"reason_code", std::to_string(
                        tb::code_value(tb::ReasonCode::OpDailySelfCheckFailed))}
                });
            } else if (item.severity == CheckSeverity::Warning) {
                logger_->warn("DailySelfCheck", "Warning check failed: " + item.name, {
                    {"detail", item.detail}
                });
            }
        } else {
            logger_->info("DailySelfCheck", "Check passed: " + item.name, {
                {"duration_ms", std::to_string(item.duration_ms)}
            });
        }

        result.checks.push_back(std::move(item));
    }

    auto wall_end = std::chrono::steady_clock::now();
    result.total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - wall_start).count();
    result.all_critical_passed = all_critical;
    result.all_passed = all_passed;

    // Summary log
    if (all_critical) {
        logger_->info("DailySelfCheck", "Self-check PASSED", {
            {"total_ms", std::to_string(result.total_duration_ms)},
            {"warnings", std::to_string(result.warnings())},
            {"reason_code", std::to_string(
                tb::code_value(tb::ReasonCode::OpDailySelfCheckPassed))}
        });
    } else {
        logger_->critical("DailySelfCheck", "Self-check FAILED — trading blocked", {
            {"total_ms", std::to_string(result.total_duration_ms)},
            {"critical_failures", std::to_string(result.critical_failures())},
            {"reason_code", std::to_string(
                tb::code_value(tb::ReasonCode::OpDailySelfCheckFailed))}
        });
    }

    // Metrics
    if (gauge_self_check_status_)
        gauge_self_check_status_->set(all_critical ? 1.0 : 0.0);
    if (counter_self_check_runs_)
        counter_self_check_runs_->increment();

    last_result_ = result;
    return result;
}

SelfCheckResult DailySelfCheck::last_result() const {
    return last_result_;
}

} // namespace tb::self_diagnosis
