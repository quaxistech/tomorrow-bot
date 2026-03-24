#pragma once
#include "decision/decision_types.hpp"
#include "strategy/strategy_types.hpp"
#include "strategy_allocator/allocation_types.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <vector>

namespace tb::decision {

/// Интерфейс движка агрегации решений
class IDecisionAggregationEngine {
public:
    virtual ~IDecisionAggregationEngine() = default;

    /// Агрегировать торговые намерения в единое решение
    virtual DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const strategy_allocator::AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty) = 0;
};

/// Комитетный движок: голосование стратегий с вето-логикой
class CommitteeDecisionEngine : public IDecisionAggregationEngine {
public:
    /// Порог одобрения (weighted_score > threshold).
    /// Повышен с 0.2 до 0.35 — требует более уверенных сигналов для входа.
    /// Это предотвращает открытие позиций на слабых/сомнительных сигналах.
    static constexpr double kDefaultThreshold = 0.35;

    CommitteeDecisionEngine(std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock);

    DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const strategy_allocator::AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty) override;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
};

} // namespace tb::decision
