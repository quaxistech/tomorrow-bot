#pragma once
#include "decision/decision_types.hpp"
#include "decision/strategy_allocation.hpp"
#include "strategy/strategy_types.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
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
    /// @param portfolio  Опциональный снимок портфеля (для drawdown-awareness, position compat)
    /// @param features   Опциональный снимок features (для execution cost modeling)
    virtual DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty,
        const std::optional<portfolio::PortfolioSnapshot>& portfolio = std::nullopt,
        const std::optional<features::FeatureSnapshot>& features = std::nullopt) = 0;
};

/// Комитетный движок профессионального уровня: голосование стратегий с вето-логикой,
/// regime-adaptive thresholds, ensemble conviction, portfolio/execution awareness
class CommitteeDecisionEngine : public IDecisionAggregationEngine {
public:
    CommitteeDecisionEngine(std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock,
                            double conviction_threshold = 0.45,
                            double dominance_threshold = 0.60,
                            AdvancedDecisionConfig advanced = {});

    DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty,
        const std::optional<portfolio::PortfolioSnapshot>& portfolio = std::nullopt,
        const std::optional<features::FeatureSnapshot>& features = std::nullopt) override;

private:
    /// Time decay: exp(-λ × age_ms), half-life from config.
    double compute_time_decay(int64_t signal_age_ns) const;

    /// Conviction penalty for round-trip execution cost.
    ExecutionCostEstimate compute_execution_cost(
        const features::FeatureSnapshot& features) const;

    /// Threshold boost when portfolio is in drawdown.
    double compute_drawdown_boost(
        const portfolio::PortfolioSnapshot& portfolio) const;

    /// Cross-state time-skew detection.
    int64_t detect_time_skew(
        const regime::RegimeSnapshot& regime,
        const uncertainty::UncertaintySnapshot& uncertainty,
        Timestamp decided_at) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    double conviction_threshold_;
    double dominance_threshold_;
    AdvancedDecisionConfig advanced_;
};

} // namespace tb::decision
