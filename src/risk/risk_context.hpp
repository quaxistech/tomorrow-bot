#pragma once
#include "risk/risk_types.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::risk {

/// Контекст для оценки риска — объединяет все входные данные для IRiskCheck
struct RiskContext {
    const strategy::TradeIntent& intent;
    const portfolio_allocator::SizingResult& sizing;
    const portfolio::PortfolioSnapshot& portfolio;
    const features::FeatureSnapshot& features;
    const execution_alpha::ExecutionAlphaResult& exec_alpha;
    const uncertainty::UncertaintySnapshot& uncertainty;
    double current_funding_rate{0.0};  ///< Текущий funding rate (8ч)
    double min_notional_usdt{0.0};     ///< Per-symbol минимальный notional (из exchange rules, 0 = fallback на kMinBitgetNotionalUsdt)
};

} // namespace tb::risk
