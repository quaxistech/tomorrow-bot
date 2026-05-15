#include "execution/recovery/recovery_manager.hpp"
#include <algorithm>

namespace tb::execution {

RecoveryManager::RecoveryManager(
    OrderRegistry& registry,
    std::shared_ptr<IOrderSubmitter> submitter,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    const ExecutionConfig& config)
    : registry_(registry)
    , submitter_(std::move(submitter))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , config_(config)
{
}

void RecoveryManager::mark_uncertain_orders() {
    auto active = registry_.active_orders();
    for (const auto& order : active) {
        if (order.state == OrderState::PendingAck || order.state == OrderState::Open) {
            registry_.force_transition(order.order_id, OrderState::UnknownRecovery,
                                       "Recovery: uncertain order detected");
            logger_->warn("RecoveryManager", "Ордер помечен для recovery",
                {{"order_id", order.order_id.get()},
                 {"prev_state", to_string(order.state)},
                 {"symbol", order.symbol.get()}});
        }
    }
}

void RecoveryManager::recover_unknown_orders() {
    // BUG-S8-14: submitter_ can be null if not initialized; guard before dereferencing
    if (!submitter_) {
        logger_->error("RecoveryManager", "submitter_ is null — cannot recover unknown orders");
        return;
    }

    std::vector<OrderRecord> unknown_orders;
    registry_.for_each([&](const OrderRecord& order) {
        if (order.state == OrderState::UnknownRecovery) {
            unknown_orders.push_back(order);
        }
    });

    for (const auto& order : unknown_orders) {
        // §21: Запрашиваем биржу о реальном статусе и реальном размере fill.
        auto fill_detail = submitter_->query_order_fill_detail(
            order.exchange_order_id, order.symbol);

        if (fill_detail.success && fill_detail.filled_qty.get() > 0.0) {
            const double recovered_qty = std::clamp(
                fill_detail.filled_qty.get(), 0.0, order.original_quantity.get());
            const bool full_fill = recovered_qty >= order.original_quantity.get() * 0.999;
            const auto recovered_state = full_fill ? OrderState::Filled : OrderState::PartiallyFilled;

            registry_.force_transition(order.order_id, recovered_state,
                                       "Recovery: confirmed fill by exchange");
            auto updated = registry_.get_order(order.order_id);
            if (updated) {
                auto u = *updated;
                if (fill_detail.fill_price.get() > 0.0) {
                    u.avg_fill_price = fill_detail.fill_price;
                }
                u.filled_quantity = Quantity(recovered_qty);
                u.remaining_quantity = Quantity(
                    std::max(0.0, u.original_quantity.get() - recovered_qty));
                u.last_updated = clock_->now();
                registry_.update_order(u);
            }

            if (!full_fill) {
                // Attempt to cancel the unfilled remainder to leave order in terminal state.
                bool cancelled = submitter_->cancel_order(order.exchange_order_id, order.symbol);
                if (cancelled) {
                    registry_.force_transition(order.order_id, OrderState::Cancelled,
                                               "Recovery: partial fill confirmed, remainder cancelled");
                }
            }

            logger_->info("RecoveryManager", "Ордер восстановлен по exchange fill detail",
                {{"order_id", order.order_id.get()},
                 {"filled_qty", std::to_string(recovered_qty)},
                 {"fill_price", std::to_string(fill_detail.fill_price.get())},
                 {"state", full_fill ? "Filled" : "PartiallyFilled"}});
        } else {
            // Unable to confirm fill — attempt actual cancel on exchange first.
            // Only transition to Cancelled after successful cancel_order() call.
            bool cancelled = submitter_->cancel_order(order.exchange_order_id, order.symbol);
            if (cancelled) {
                registry_.force_transition(order.order_id, OrderState::Cancelled,
                                           "Recovery: confirmed cancelled on exchange");
                logger_->info("RecoveryManager", "Ордер отменён на бирже и восстановлен как Cancelled",
                    {{"order_id", order.order_id.get()},
                     {"symbol", order.symbol.get()}});
            } else {
                // Cancel failed — keep in UnknownRecovery for manual resolution.
                // Do NOT assume cancelled without exchange confirmation.
                logger_->warn("RecoveryManager",
                    "Ордер остаётся в UnknownRecovery: cancel не подтверждён биржей",
                    {{"order_id", order.order_id.get()},
                     {"symbol", order.symbol.get()}});
            }
        }
    }

    if (!unknown_orders.empty() && metrics_) {
        metrics_->counter("execution_recovery_total", {})->increment(
            static_cast<double>(unknown_orders.size()));
    }
}

ReconciliationResult RecoveryManager::run_reconciliation() {
    ReconciliationResult result;

    logger_->info("RecoveryManager", "Начало reconciliation");

    // §21: Step 1 — пометить uncertain ордера
    mark_uncertain_orders();

    // §21: Step 2 — восстановить ордера
    std::vector<OrderRecord> before;
    registry_.for_each([&](const OrderRecord& o) {
        if (o.state == OrderState::UnknownRecovery) {
            before.push_back(o);
        }
    });

    recover_unknown_orders();

    // Подсчитать результаты
    for (const auto& o : before) {
        auto after = registry_.get_order(o.order_id);
        if (!after) continue;

        if (after->state == OrderState::Filled) {
            result.orders_recovered++;
        } else if (after->state == OrderState::Cancelled) {
            result.orders_force_cancelled++;
        } else {
            result.anomalies_detected++;
            result.anomaly_descriptions.push_back(
                "Order " + o.order_id.get() + " still in " + to_string(after->state));
        }
    }

    // Step 3 — resolve stale CancelPending.
    // 3× multiplier: initial cancel_confirmation_timeout covers normal round-trip,
    // plus 2 additional round-trips to account for exchange processing delay and
    // network jitter. Orders stuck beyond 3× are assumed lost and force-cancelled.
    int stale_resolved = force_resolve_stale_cancels(
        config_.cancel_confirmation_timeout_ms * 3);
    result.orders_force_cancelled += stale_resolved;

    result.success = (result.anomalies_detected == 0);

    logger_->info("RecoveryManager", "Reconciliation завершён",
        {{"recovered", std::to_string(result.orders_recovered)},
         {"force_cancelled", std::to_string(result.orders_force_cancelled)},
         {"anomalies", std::to_string(result.anomalies_detected)},
         {"success", result.success ? "true" : "false"}});

    if (metrics_) {
        metrics_->counter("execution_reconciliation_total", {})->increment();
        if (result.anomalies_detected > 0) {
            metrics_->counter("execution_anomalies_total", {})->increment(
                static_cast<double>(result.anomalies_detected));
        }
    }

    return result;
}

int RecoveryManager::force_resolve_stale_cancels(int64_t max_stale_ms) {
    int resolved = 0;
    std::vector<OrderRecord> stale;

    registry_.for_each([&](const OrderRecord& order) {
        if (order.state == OrderState::CancelPending) {
            auto time_ms = registry_.time_in_state_ms(order.order_id);
            if (time_ms && *time_ms > max_stale_ms) {
                stale.push_back(order);
            }
        }
    });

    for (const auto& order : stale) {
        registry_.force_transition(order.order_id, OrderState::Cancelled,
                                   "Recovery: stale CancelPending → force cancel");
        logger_->warn("RecoveryManager", "CancelPending зависший → force Cancelled",
            {{"order_id", order.order_id.get()},
             {"symbol", order.symbol.get()}});
        ++resolved;
    }

    return resolved;
}

int RecoveryManager::resolve_recovery_timeout(int64_t max_recovery_ms) {
    int resolved = 0;
    std::vector<OrderRecord> stuck;

    registry_.for_each([&](const OrderRecord& order) {
        if (order.state == OrderState::UnknownRecovery) {
            auto time_ms = registry_.time_in_state_ms(order.order_id);
            if (time_ms && *time_ms > max_recovery_ms) {
                stuck.push_back(order);
            }
        }
    });

    for (const auto& order : stuck) {
        registry_.force_transition(order.order_id, OrderState::Cancelled,
                                   "Recovery timeout → force cancel");
        logger_->warn("RecoveryManager", "UnknownRecovery timeout → Cancelled",
            {{"order_id", order.order_id.get()}});
        ++resolved;
    }

    return resolved;
}

} // namespace tb::execution
