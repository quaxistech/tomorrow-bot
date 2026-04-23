#pragma once
/**
 * @file execution_engine.hpp
 * @brief Execution Engine — оркестратор исполнения (§1-§39 ТЗ)
 *
 * Единственный шлюз исполнения (§4).
 * Делегирует подсистемам: OrderRegistry, ExecutionPlanner,
 * FillProcessor, CancelManager, RecoveryManager, ExecutionMetrics.
 */

#include "execution/order_types.hpp"
#include "execution/order_fsm.hpp"
#include "execution/execution_types.hpp"
#include "execution/execution_config.hpp"
#include "execution/order_submitter.hpp"
#include "execution/orders/order_registry.hpp"
#include "execution/planner/execution_planner.hpp"
#include "execution/fills/fill_processor.hpp"
#include "execution/cancel/cancel_manager.hpp"
#include "execution/recovery/recovery_manager.hpp"
#include "execution/telemetry/execution_metrics.hpp"
#include "strategy/strategy_types.hpp"
#include "risk/risk_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::execution {

// ═══════════════════════════════════════════════════════════════════════════════
// §29: ExecutionEngine — главный оркестратор
// ═══════════════════════════════════════════════════════════════════════════════

/// Движок исполнения ордеров (§1-§39 ТЗ)
///
/// Модульная архитектура (§26):
/// - OrderRegistry:     хранение ордеров, FSM, дедупликация
/// - ExecutionPlanner:  intent → plan (тип ордера, таймаут, fallback)
/// - FillProcessor:     обработка fills, обновление портфеля
/// - CancelManager:     отмены (timeout, context, emergency)
/// - RecoveryManager:   reconciliation, восстановление state
/// - ExecutionMetrics:  телеметрия исполнения
class ExecutionEngine {
public:
    ExecutionEngine(std::shared_ptr<IOrderSubmitter> submitter,
                    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
                    std::shared_ptr<logging::ILogger> logger,
                    std::shared_ptr<clock::IClock> clock,
                    std::shared_ptr<metrics::IMetricsRegistry> metrics,
                    ExecutionConfig config = ExecutionConfig{});

    // ─── §29.1: Основная точка входа ─────────────────────────────────────

    /// Принять intent к исполнению
    [[nodiscard]] Result<OrderId> execute(
        const strategy::TradeIntent& intent,
        const risk::RiskDecision& risk_decision,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const uncertainty::UncertaintySnapshot& uncertainty);

    // ─── §29.2: Команды управления ───────────────────────────────────────

    /// Отменить конкретный ордер
    VoidResult cancel(const OrderId& order_id);

    /// Отменить все ордера для символа (§8)
    std::vector<OrderId> cancel_all_for_symbol(const Symbol& symbol);

    /// Отменить все ордера глобально (§8)
    std::vector<OrderId> cancel_all();

    /// Emergency flatten символа (§8)
    VoidResult emergency_flatten_symbol(const Symbol& symbol);

    // ─── §29.3: События от биржи ─────────────────────────────────────────

    /// Обновить состояние ордера (от биржи)
    void on_order_update(const OrderId& order_id, OrderState new_state,
                         Quantity filled_qty = Quantity(0.0),
                         Price fill_price = Price(0.0));

    /// Обработать fill event (partial или full)
    void on_fill_event(const FillEvent& fill);

    // ─── §29.4: Сервисные методы ─────────────────────────────────────────

    /// Получить запись ордера
    std::optional<OrderRecord> get_order(const OrderId& order_id) const;

    /// Все активные ордера
    std::vector<OrderRecord> active_orders() const;

    /// Все ордера для символа
    [[nodiscard]] std::vector<OrderRecord> orders_for_symbol(const Symbol& symbol) const;

    /// Проверка дублирования (§22)
    [[nodiscard]] bool is_duplicate(const strategy::TradeIntent& intent);

    /// Проверить ордера с истекшим timeout и отменить (§18)
    std::vector<OrderId> cancel_timed_out_orders(int64_t max_open_duration_ms);

    /// Удалить ордера в терминальных состояниях старше max_age_ns
    size_t cleanup_terminal_orders(int64_t max_age_ns);

    /// Установить leverage для фьючерсов
    void set_leverage(double leverage) { leverage_ = std::max(leverage, 1.0); }

    /// ИСПРАВЛЕНИЕ H5: Обновить min notional из exchange rules (symbol-specific)
    void set_min_notional_usdt(double min_usdt) {
        if (min_usdt > 0.0) config_.min_notional_usdt = min_usdt;
    }

    /// Получить статистику fills для ордера
    [[nodiscard]] std::optional<OrderExecutionInfo> get_execution_info(const OrderId& order_id) const;

    /// Установить partial fill policy по умолчанию
    void set_default_fill_policy(PartialFillPolicy policy);

    /// Выполнить reconciliation (§21)
    ReconciliationResult run_reconciliation();

    /// Получить метрики исполнения (§23)
    ExecutionStats execution_stats() const;

    /// Доступ к подсистемам (для pipeline)
    OrderRegistry& registry() { return registry_; }
    const ExecutionConfig& config() const { return config_; }

private:
    /// Создать запись ордера из интента, risk decision и плана
    OrderRecord create_order_record(const strategy::TradeIntent& intent,
                                     const risk::RiskDecision& risk_decision,
                                     const execution_alpha::ExecutionAlphaResult& exec_alpha,
                                     const ExecutionPlan& plan);

    /// Валидировать intent + risk + uncertainty (§5 steps 1-2)
    VoidResult validate_inputs(const strategy::TradeIntent& intent,
                               const risk::RiskDecision& risk_decision,
                               const execution_alpha::ExecutionAlphaResult& exec_alpha,
                               const uncertainty::UncertaintySnapshot& uncertainty);

    /// Margin reservation для открывающего ордера (Long или Short)
    bool try_reserve_margin(OrderRecord& order);

    ExecutionConfig config_;
    std::shared_ptr<IOrderSubmitter> submitter_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_registry_;

    // Подсистемы (§26)
    OrderRegistry registry_;
    ExecutionPlanner planner_;
    FillProcessor fill_processor_;
    CancelManager cancel_manager_;
    RecoveryManager recovery_manager_;
    ExecutionMetrics exec_metrics_;

    double leverage_{1.0};
    PartialFillPolicy default_fill_policy_{PartialFillPolicy::WaitForFull};
    mutable std::mutex execute_mutex_;  // H-4: serialize execute() to prevent dedup TOCTOU
};

} // namespace tb::execution
