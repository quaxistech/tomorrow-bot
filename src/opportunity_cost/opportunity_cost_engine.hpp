#pragma once
#include "opportunity_cost/opportunity_cost_types.hpp"
#include "strategy/strategy_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

namespace tb::opportunity_cost {

/// Интерфейс движка стоимости упущенных возможностей
class IOpportunityCostEngine {
public:
    virtual ~IOpportunityCostEngine() = default;

    /// Оценить стоимость возможности для данного намерения
    virtual OpportunityCostResult evaluate(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        double current_exposure_pct,
        double conviction_threshold
    ) = 0;
};

/// Реализация на основе правил
class RuleBasedOpportunityCost : public IOpportunityCostEngine {
public:
    struct Config {
        double min_net_expected_bps{0.0};       ///< Мин чистый ожидаемый доход для входа
        double high_exposure_threshold{0.8};     ///< Порог высокой экспозиции
        double high_exposure_min_conviction{0.7}; ///< Мин conviction при высокой экспозиции
        double execute_min_net_bps{20.0};        ///< Мин чистый доход для немедленного исполнения
    };

    RuleBasedOpportunityCost(Config config,
                             std::shared_ptr<logging::ILogger> logger,
                             std::shared_ptr<clock::IClock> clock);

    OpportunityCostResult evaluate(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        double current_exposure_pct,
        double conviction_threshold
    ) override;

private:
    /// Рассчитать балл возможности
    OpportunityScore compute_score(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        double current_exposure_pct) const;

    /// Определить действие
    OpportunityAction determine_action(
        const OpportunityScore& score,
        double current_exposure_pct,
        double conviction,
        double conviction_threshold) const;

    Config config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
};

} // namespace tb::opportunity_cost
