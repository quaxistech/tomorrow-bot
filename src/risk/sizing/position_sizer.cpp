#include "risk/sizing/position_sizer.hpp"
#include <algorithm>
#include <cmath>

namespace tb::risk {

SizingAdjustment PositionSizer::compute_adjustment(
    Quantity proposed_size,
    double price,
    double equity,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& features) const
{
    SizingAdjustment result;
    result.recommended_size = proposed_size;
    result.max_allowed_size = proposed_size;
    result.reduction_factor = 1.0;

    if (proposed_size.get() <= 0.0 || price <= 0.0 || equity <= 0.0) return result;

    double factor = 1.0;

    // 1. Volatility-aware reduction
    double vol_f = volatility_factor(features);
    if (vol_f < 1.0) {
        factor *= vol_f;
        result.reasons.push_back("volatility_elevated (factor=" + std::to_string(vol_f) + ")");
    }

    // 2. Liquidity-aware reduction
    double liq_f = liquidity_factor(features);
    if (liq_f < 1.0) {
        factor *= liq_f;
        result.reasons.push_back("poor_liquidity (factor=" + std::to_string(liq_f) + ")");
    }

    // 3. Drawdown-aware reduction
    double dd_f = drawdown_factor(portfolio);
    if (dd_f < 1.0) {
        factor *= dd_f;
        result.reasons.push_back("drawdown_proximity (factor=" + std::to_string(dd_f) + ")");
    }

    // 4. Max notional cap
    double notional = proposed_size.get() * price * factor;
    if (notional > config_.max_position_notional) {
        double notional_factor = config_.max_position_notional / notional;
        factor *= notional_factor;
        result.reasons.push_back("exceeds_max_notional");
    }

    // 5. Max risk per trade (% of equity)
    double risk_pct = (proposed_size.get() * price * factor) / equity * 100.0;
    if (risk_pct > config_.max_risk_per_trade_pct) {
        double risk_factor = config_.max_risk_per_trade_pct / risk_pct;
        factor *= risk_factor;
        result.reasons.push_back("exceeds_max_risk_per_trade_pct");
    }

    factor = std::clamp(factor, 0.0, 1.0);
    result.reduction_factor = factor;
    result.recommended_size = Quantity(proposed_size.get() * factor);
    result.max_allowed_size = Quantity(proposed_size.get() * factor);

    return result;
}

double PositionSizer::volatility_factor(const features::FeatureSnapshot& features) const {
    // ATR-based: если волатильность высокая, уменьшаем
    double atr_pct = 0.0;
    if (features.technical.atr_14 > 0.0 && features.technical.sma_20 > 0.0) {
        atr_pct = features.technical.atr_14 / features.technical.sma_20 * 100.0;
    }

    // Sweet spot: ATR < 1% = factor 1.0, ATR 1-3% = linear decay, ATR > 3% = 0.4
    if (atr_pct <= 1.0) return 1.0;
    if (atr_pct >= 3.0) return 0.4;
    return 1.0 - 0.3 * (atr_pct - 1.0);
}

double PositionSizer::liquidity_factor(const features::FeatureSnapshot& features) const {
    if (!features.microstructure.liquidity_valid) return 0.8;

    double depth = features.microstructure.bid_depth_5_notional +
                   features.microstructure.ask_depth_5_notional;

    // Нормализация: depth >= min_liquidity => 1.0, depth < min/2 => 0.3
    double min_depth = config_.min_liquidity_depth;
    if (depth >= min_depth) return 1.0;
    if (depth <= min_depth * 0.5) return 0.3;
    return 0.3 + 0.7 * (depth - min_depth * 0.5) / (min_depth * 0.5);
}

double PositionSizer::drawdown_factor(const portfolio::PortfolioSnapshot& portfolio) const {
    double dd = portfolio.pnl.current_drawdown_pct;
    double max_dd = config_.max_drawdown_pct;

    if (dd <= 0.0 || max_dd <= 0.0) return 1.0;

    double utilization = dd / max_dd;
    if (utilization <= 0.5) return 1.0;
    if (utilization >= 0.9) return 0.3;
    // Linear decay from 1.0 to 0.3 between 50-90% utilization
    return 1.0 - 0.7 * (utilization - 0.5) / 0.4;
}

} // namespace tb::risk
