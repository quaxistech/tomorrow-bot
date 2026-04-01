#include "pair_ranker.hpp"
#include <algorithm>
#include <cmath>

namespace tb::scanner {

SymbolScore PairRanker::compute(const SymbolFeatures& features,
                                const TrapAggregateResult& traps,
                                const MarketSnapshot& snapshot) const {
    SymbolScore s;

    // §8: Weighted composite score
    s.liquidity_score = features.liquidity.score;
    s.spread_score = features.spread.score;
    s.volatility_score = features.volatility.score;
    s.orderbook_score = features.orderbook.score;
    s.trend_quality_score = features.trend_quality.score;

    // Execution quality: combines spread + depth + vol/spread ratio
    s.execution_quality_score = (features.spread.score * 0.4 +
                                  std::min(features.liquidity.total_depth_near_mid / 200'000.0, 1.0) * 0.3 +
                                  std::min(features.volatility.vol_to_spread_ratio / 10.0, 1.0) * 0.3);

    // Penalties
    s.trap_risk_penalty = traps.total_risk;

    // Funding extreme penalty
    double abs_funding = std::abs(snapshot.funding_rate);
    if (abs_funding > cfg_.funding_extreme_threshold) {
        double excess = (abs_funding - cfg_.funding_extreme_threshold) / cfg_.funding_extreme_threshold;
        s.funding_penalty = std::min(excess * 0.5, 1.0);
    }

    // §8: score = weighted sum of scores - weighted penalties
    double positive =
        cfg_.weight_liquidity * s.liquidity_score +
        cfg_.weight_spread * s.spread_score +
        cfg_.weight_volatility * s.volatility_score +
        cfg_.weight_orderbook * s.orderbook_score +
        cfg_.weight_trend_quality * s.trend_quality_score +
        cfg_.weight_execution_quality * s.execution_quality_score;

    double negative =
        cfg_.weight_trap_risk * s.trap_risk_penalty +
        cfg_.weight_funding_extreme * s.funding_penalty;

    s.total = std::max(0.0, positive - negative);

    // §15: Explainability — bonus/penalty reasons
    if (s.liquidity_score > 0.8) s.bonus_reasons.push_back("high_liquidity");
    if (s.spread_score > 0.8) s.bonus_reasons.push_back("tight_spread");
    if (s.volatility_score > 0.7) s.bonus_reasons.push_back("good_volatility");
    if (s.orderbook_score > 0.7) s.bonus_reasons.push_back("stable_orderbook");
    if (s.trend_quality_score > 0.6) s.bonus_reasons.push_back("good_microtrend");
    if (features.volatility.has_impulse) s.bonus_reasons.push_back("active_impulse");

    if (s.trap_risk_penalty > 0.3) s.penalty_reasons.push_back("moderate_trap_risk");
    if (s.trap_risk_penalty > 0.6) s.penalty_reasons.push_back("high_trap_risk");
    if (s.funding_penalty > 0.2) s.penalty_reasons.push_back("extreme_funding_rate");
    if (features.anomaly.volume_spike) s.penalty_reasons.push_back("volume_anomaly");
    if (features.anomaly.oi_price_divergence) s.penalty_reasons.push_back("oi_price_divergence");

    // Confidence: how much data we had and how consistent signals are
    int strong_signals = 0;
    if (s.liquidity_score > 0.5) strong_signals++;
    if (s.spread_score > 0.5) strong_signals++;
    if (s.volatility_score > 0.3) strong_signals++;
    if (s.orderbook_score > 0.4) strong_signals++;
    if (s.trend_quality_score > 0.3) strong_signals++;

    s.confidence = std::min(strong_signals / 5.0, 1.0);
    if (s.trap_risk_penalty > 0.5) s.confidence *= 0.7;

    return s;
}

} // namespace tb::scanner
