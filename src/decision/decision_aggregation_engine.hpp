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
    /// Порог одобрения (conviction > threshold × uncertainty_multiplier).
    /// Снижен до 0.28 для spot-торговли с малым капиталом и tight stop-loss.
    /// Прежний 0.35 с uncertainty_multiplier до 1.3 давал эффективный порог 0.46,
    /// что блокировало даже обоснованные сигналы mean reversion.
    static constexpr double kDefaultThreshold = 0.28;

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
