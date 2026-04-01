#pragma once
#include "decision/decision_types.hpp"
#include "strategy/strategy_types.hpp"
#include "strategy_allocator/allocation_types.hpp"
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
        const strategy_allocator::AllocationResult& allocation,
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
                            double conviction_threshold = 0.28,
                            double dominance_threshold = 0.60,
                            AdvancedDecisionConfig advanced = {});

    DecisionRecord aggregate(
        const Symbol& symbol,
        const std::vector<strategy::TradeIntent>& intents,
        const strategy_allocator::AllocationResult& allocation,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty,
        const std::optional<portfolio::PortfolioSnapshot>& portfolio = std::nullopt,
        const std::optional<features::FeatureSnapshot>& features = std::nullopt) override;

private:
    /// Множитель порога conviction по режиму рынка
    double compute_regime_threshold_factor(regime::DetailedRegime regime) const;

    /// Множитель порога доминирования по режиму рынка
    double compute_regime_dominance_threshold(regime::DetailedRegime regime) const;

    /// Time decay: exp(-λ × age_ms), half-life из конфига
    double compute_time_decay(int64_t signal_age_ns) const;

    /// Пенальти conviction за стоимость исполнения
    ExecutionCostEstimate compute_execution_cost(
        const features::FeatureSnapshot& features) const;

    /// Повышение порога при просадке портфеля
    double compute_drawdown_boost(
        const portfolio::PortfolioSnapshot& portfolio) const;

    /// Ансамблевый бонус conviction при согласии нескольких стратегий
    EnsembleMetrics compute_ensemble_metrics(
        const std::vector<struct ScoredIntent>& scored,
        Side winning_side) const;

    /// Детекция рассинхронизации входных состояний
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
