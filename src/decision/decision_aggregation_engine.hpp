#pragma once
#include "decision/decision_types.hpp"
#include "strategy/strategy_types.hpp"
#include "strategy_allocator/allocation_types.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "ai/ai_advisory_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <vector>
#include <optional>

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
        const uncertainty::UncertaintySnapshot& uncertainty,
        const std::optional<ai::AIAdvisoryResult>& ai_advisory = std::nullopt) = 0;
};

/// Комитетный движок: голосование стратегий с вето-логикой
class CommitteeDecisionEngine : public IDecisionAggregationEngine {
public:
    CommitteeDecisionEngine(std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock,
                            double conviction_threshold = 0.28,
                            double dominance_threshold = 0.60);

    DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const strategy_allocator::AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty,
        const std::optional<ai::AIAdvisoryResult>& ai_advisory = std::nullopt) override;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    double conviction_threshold_;
    double dominance_threshold_;
};

} // namespace tb::decision
