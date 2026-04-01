#pragma once
/**
 * @file execution_planner.hpp
 * @brief Execution Planner (§13-§14 ТЗ)
 *
 * Преобразует OrderIntent + RiskDecision + MarketExecutionContext в ExecutionPlan.
 * Отвечает на: какой тип ордера, какая цена, нужен ли fallback/timeout,
 * reduce-only, разбивать ли на части.
 */

#include "execution/execution_types.hpp"
#include "execution/execution_config.hpp"
#include "strategy/strategy_types.hpp"
#include "risk/risk_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "logging/logger.hpp"
#include <memory>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::execution {

/// Execution Planner — преобразует intent в конкретный план исполнения
class ExecutionPlanner {
public:
    explicit ExecutionPlanner(const ExecutionConfig& config,
                              std::shared_ptr<logging::ILogger> logger);

    /// Создать план исполнения на основе входных данных
    /// @param intent  Торговое намерение от Strategy Engine
    /// @param risk    Решение Risk Engine
    /// @param exec_alpha  Рекомендации execution alpha
    /// @param market  Рыночный контекст (спред, глубина, etc.)
    /// @param uncertainty  Уровень неопределённости
    ExecutionPlan plan(const strategy::TradeIntent& intent,
                       const risk::RiskDecision& risk,
                       const execution_alpha::ExecutionAlphaResult& exec_alpha,
                       const MarketExecutionContext& market,
                       const uncertainty::UncertaintySnapshot& uncertainty);

    /// Определить тип действия из intent
    ExecutionAction classify_action(const strategy::TradeIntent& intent) const;

private:
    /// Выбрать стиль исполнения
    PlannedExecutionStyle choose_style(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const MarketExecutionContext& market,
        ExecutionAction action) const;

    /// Выбрать тип ордера из стиля
    OrderType style_to_order_type(PlannedExecutionStyle style) const;

    /// Выбрать TimeInForce из стиля
    TimeInForce style_to_tif(PlannedExecutionStyle style) const;

    /// Рассчитать цену для limit-ордера
    Price compute_limit_price(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const MarketExecutionContext& market) const;

    /// Определить таймаут для ордера
    int64_t compute_timeout(ExecutionAction action, PlannedExecutionStyle style) const;

    /// Определить, нужен ли reduce-only
    bool should_reduce_only(const strategy::TradeIntent& intent,
                            ExecutionAction action) const;

    const ExecutionConfig& config_;
    std::shared_ptr<logging::ILogger> logger_;
};

} // namespace tb::execution
