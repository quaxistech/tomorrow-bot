#pragma once
/**
 * @file market_context.hpp
 * @brief Оценка качества рыночного контекста для Strategy Engine (§13 ТЗ)
 */

#include "strategy/strategy_types.hpp"
#include "strategy/strategy_config.hpp"
#include "strategy/state/setup_models.hpp"
#include "features/feature_snapshot.hpp"

namespace tb::strategy {

/// Оценивает качество рыночного контекста для скальпинга
class MarketContextEvaluator {
public:
    explicit MarketContextEvaluator(const ScalpStrategyConfig& cfg) : cfg_(cfg) {}

    /// Полная оценка контекста
    MarketContextResult evaluate(const StrategyContext& ctx) const;

private:
    const ScalpStrategyConfig& cfg_;
};

} // namespace tb::strategy
