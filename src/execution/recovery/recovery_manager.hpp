#pragma once
/**
 * @file recovery_manager.hpp
 * @brief Recovery & Reconciliation Manager (§21 ТЗ)
 *
 * Восстанавливает согласованное состояние после сбоев.
 * Unknown state → reconciliation, не optimistic assumptions.
 */

#include "execution/orders/order_registry.hpp"
#include "execution/execution_config.hpp"
#include "execution/order_submitter.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <vector>

namespace tb::execution {

/// Результат reconciliation
struct ReconciliationResult {
    int orders_recovered{0};
    int orders_force_cancelled{0};
    int anomalies_detected{0};
    std::vector<std::string> anomaly_descriptions;
    bool success{true};
};

/// Recovery & Reconciliation Manager
class RecoveryManager {
public:
    RecoveryManager(OrderRegistry& registry,
                    std::shared_ptr<IOrderSubmitter> submitter,
                    std::shared_ptr<logging::ILogger> logger,
                    std::shared_ptr<clock::IClock> clock,
                    std::shared_ptr<metrics::IMetricsRegistry> metrics,
                    const ExecutionConfig& config);

    /// Перевести все uncertain-ордера в UnknownRecovery (§33)
    void mark_uncertain_orders();

    /// Попытаться восстановить ордер в неизвестном состоянии
    /// Запрашивает биржу о реальном состоянии (через IOrderSubmitter)
    /// Примечание: query_order_fill_price используется как proxy-check
    void recover_unknown_orders();

    /// Выполнить полный цикл reconciliation
    ReconciliationResult run_reconciliation();

    /// Принудительно закрыть все CancelPending ордера, которые зависли
    /// (давно в CancelPending без подтверждения)
    int force_resolve_stale_cancels(int64_t max_stale_ms);

    /// Очистить ордера в неопределённом состоянии после таймаута
    int resolve_recovery_timeout(int64_t max_recovery_ms);

private:
    OrderRegistry& registry_;
    std::shared_ptr<IOrderSubmitter> submitter_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    const ExecutionConfig& config_;
};

} // namespace tb::execution
