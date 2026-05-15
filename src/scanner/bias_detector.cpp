#include "bias_detector.hpp"
#include <cmath>

namespace tb::scanner {

BiasDetector::BiasResult BiasDetector::detect(const MarketSnapshot& snapshot,
                                               const SymbolFeatures& features,
                                               const TrapAggregateResult& traps) const {
    BiasResult result;

    // §9: Bias определяется на основании:
    // - микротренда
    // - order book imbalance
    // - подтверждения движением
    // - качества импульса
    // - отсутствия явных ловушек
    // - оценки adverse selection

    // Если ловушки слишком сильны → NEUTRAL / DO_NOT_TRADE
    if (traps.total_risk > 0.7) {
        result.direction = BiasDirection::Neutral;
        result.confidence = 0.0;
        result.reasons.push_back("high_trap_risk_prevents_bias");
        return result;
    }

    // Факторы bias (каждый голосует +1 LONG, -1 SHORT, или 0)
    double bias_signal = 0.0;
    double total_weight = 0.0;

    // 1. Микротренд (вес 0.35)
    {
        double dir = features.trend_quality.micro_trend_direction;
        double strength = features.trend_quality.micro_trend_strength;
        if (!std::isfinite(dir)) dir = 0.0;
        if (!std::isfinite(strength)) strength = 0.0;
        if (strength > 0.2) {
            bias_signal += dir * strength * 0.35;
            total_weight += 0.35;
            if (dir > 0.3) result.reasons.push_back("upward_microtrend");
            else if (dir < -0.3) result.reasons.push_back("downward_microtrend");
        }
    }

    // 2. Order book imbalance (вес 0.25)
    {
        double imb = features.orderbook.imbalance_5;
        if (!std::isfinite(imb)) imb = 0.0;
        if (std::abs(imb) > 0.15) {
            bias_signal += imb * 0.25;
            total_weight += 0.25;
            if (imb > 0.2) result.reasons.push_back("bid_side_dominant");
            else if (imb < -0.2) result.reasons.push_back("ask_side_dominant");
        }
    }

    // 3. Price momentum (вес 0.20) — recent price velocity
    {
        const auto& candles = snapshot.candles;
        if (!candles.empty() && candles.size() >= 3) {
            double recent_change = candles.back().close - candles[candles.size()-3].close;
            double mid = snapshot.orderbook.mid_price();
            if (mid > 0.0 && std::isfinite(recent_change)) {
                double change_pct = recent_change / mid;
                if (std::abs(change_pct) > 0.0005) {  // > 5bps
                    double signal = (change_pct > 0) ? 1.0 : -1.0;
                    bias_signal += signal * std::min(std::abs(change_pct) * 200.0, 1.0) * 0.20;
                    total_weight += 0.20;
                    if (change_pct > 0) result.reasons.push_back("upward_momentum");
                    else result.reasons.push_back("downward_momentum");
                }
            }
        }
    }

    // 4. Impulse quality (вес 0.10)
    {
        if (features.volatility.has_impulse && features.volatility.impulse_quality > 0.3) {
            // Direction of impulse from last few candles
            const auto& candles = snapshot.candles;
            if (candles.size() >= 3) {
                double last_move = candles.back().close - candles[candles.size()-3].open;
                double dir = (last_move > 0) ? 1.0 : -1.0;
                double iq = features.volatility.impulse_quality;
                if (std::isfinite(iq)) {
                    bias_signal += dir * iq * 0.10;
                    total_weight += 0.10;
                    result.reasons.push_back("strong_impulse");
                }
            }
        }
    }

    // 5. Funding rate contrarian (вес 0.10) — extreme funding suggests crowd on one side
    {
        double fr = snapshot.funding_rate;
        if (!std::isfinite(fr)) fr = 0.0;
        if (std::abs(fr) > cfg_.funding_extreme_threshold * 0.5) {
            // Contrarian: if longs crowded (positive funding), bias SHORT; vice versa
            double contrarian = (fr > 0) ? -1.0 : 1.0;
            double weight = std::min(std::abs(fr) / cfg_.funding_extreme_threshold, 1.0) * 0.10;
            bias_signal += contrarian * weight;
            total_weight += 0.10;
            if (fr > 0) result.reasons.push_back("funding_suggests_short");
            else result.reasons.push_back("funding_suggests_long");
        }
    }

    // BUG-S11-01: dividing by total_weight (actual fired weight) inflates weak isolated signals:
    // a single 0.35-weight signal at full strength gives 0.35/0.35=1.0, exceeding every threshold.
    // All component weights sum to at most 1.0, so bias_signal is already in [-1.0, +1.0].
    // Normalize by max possible weight (1.0) = keep bias_signal as-is.
    double normalized_bias = (total_weight > 0.0) ? bias_signal : 0.0;
    if (!std::isfinite(normalized_bias)) normalized_bias = 0.0;

    if (normalized_bias > cfg_.bias_neutral_zone) {
        result.direction = BiasDirection::Long;
        result.confidence = std::min(std::abs(normalized_bias), 1.0);
    } else if (normalized_bias < -cfg_.bias_neutral_zone) {
        result.direction = BiasDirection::Short;
        result.confidence = std::min(std::abs(normalized_bias), 1.0);
    } else {
        result.direction = BiasDirection::Neutral;
        result.confidence = 1.0 - std::abs(normalized_bias) / cfg_.bias_neutral_zone;
        result.reasons.push_back("mixed_signals_neutral");
    }

    // Reduce confidence if too many traps
    if (traps.total_risk > 0.3) {
        result.confidence *= (1.0 - traps.total_risk * 0.5);
    }

    // Below minimum confidence → force neutral
    if (result.confidence < cfg_.bias_min_confidence &&
        result.direction != BiasDirection::Neutral) {
        result.direction = BiasDirection::Neutral;
        result.reasons.push_back("confidence_below_threshold");
    }

    return result;
}

} // namespace tb::scanner
