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

    // Execution quality: combines spread + depth + vol/spread ratio.
    // B13.1/B13.2: saturation thresholds можно вынести в ScannerConfig для
    // per-market калибровки.
    constexpr double kDepthSaturationUsdt = 200'000.0;
    constexpr double kVolSpreadSaturation = 10.0;
    s.execution_quality_score = (features.spread.score * 0.4 +
        std::min(features.liquidity.total_depth_near_mid / kDepthSaturationUsdt, 1.0) * 0.3 +
        std::min(features.volatility.vol_to_spread_ratio / kVolSpreadSaturation, 1.0) * 0.3);

    // Penalties
    s.trap_risk_penalty = traps.total_risk;

    // Funding extreme penalty
    double funding_rate = snapshot.funding_rate;
    if (!std::isfinite(funding_rate)) funding_rate = 0.0;
    double abs_funding = std::abs(funding_rate);
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

    s.total = std::isfinite(positive - negative) ? std::max(0.0, positive - negative) : 0.0;

    // EDGE-31 Scalpability composite — заменяет total если scalping включён.
    if (cfg_.scalping.enabled) {
        const auto& sp = cfg_.scalping;

        // Soft components (each 0-1)
        double spread_comp = 1.0 - std::clamp(features.spread.spread_bps / sp.hg_max_spread_bps, 0.0, 1.0);
        double depth_comp = features.liquidity.book_depth_1pct_usdt > 0.0
            ? std::log1p(features.liquidity.book_depth_1pct_usdt / sp.hg_min_book_depth_1pct_usdt) / std::log1p(10.0)
            : 0.0;
        depth_comp = std::clamp(depth_comp, 0.0, 1.0);
        double resiliency_comp = std::clamp(features.orderbook.resiliency_score, 0.0, 1.0);
        double vol_quality_comp = std::clamp(features.volatility.vol_quality_score, 0.0, 1.0);
        // Trade flow: trade_count_1m saturates at 200. B13.2: эмпирический порог.
        constexpr double kTradeFlowSaturation = 200.0;
        double trade_flow_comp = std::clamp(features.trend_quality.trade_count_1m / kTradeFlowSaturation, 0.0, 1.0);
        // Microstructure: body × (1-wick)
        double micro_comp = std::clamp(features.trend_quality.body_to_range_ratio, 0.0, 1.0)
                          * std::max(0.0, 1.0 - features.trend_quality.wick_ratio);
        // Execution quality: 1 - slippage / max
        double exec_q_comp = sp.hg_max_slippage_at_10usdt_bps > 0.0
            ? std::clamp(1.0 - features.orderbook.slippage_at_10usdt_bps / sp.hg_max_slippage_at_10usdt_bps, 0.0, 1.0)
            : 0.5;
        // B5.2: regime_match сейчас аппроксимируется через momentum_persistence.
        // Полный regime classifier требует pass'a regime snapshot — оставлено
        // как простой proxy чтобы не блокировать ranking, но это документировано.
        double regime_match_comp = std::clamp(features.trend_quality.momentum_persistence, 0.0, 1.0);

        double scalp_positive =
            sp.w_spread * spread_comp +
            sp.w_depth * depth_comp +
            sp.w_resiliency * resiliency_comp +
            sp.w_vol_quality * vol_quality_comp +
            sp.w_trade_flow * trade_flow_comp +
            sp.w_micro_structure * micro_comp +
            sp.w_execution_quality * exec_q_comp +
            sp.w_regime_match * regime_match_comp;

        double scalp_negative =
            sp.p_trap_risk * s.trap_risk_penalty +
            sp.p_funding_drift * s.funding_penalty;

        s.scalpability_score = std::isfinite(scalp_positive - scalp_negative)
            ? std::clamp(scalp_positive - scalp_negative, 0.0, 1.0)
            : 0.0;
        s.total = s.scalpability_score;  // override legacy total

        // Regime tag classifier (inline simplified)
        if (features.volatility.vol_quality_score > 0.5 &&
            features.trend_quality.momentum_persistence > 0.6 &&
            features.trend_quality.body_to_range_ratio > 0.5) {
            s.regime_tag = RegimeTag::Momentum;
            s.bonus_reasons.push_back("momentum_setup");
        } else if (features.volatility.vol_quality_score > 0.4 &&
                   features.trend_quality.micro_trend_strength < 0.3) {
            s.regime_tag = RegimeTag::MeanReversion;
            s.bonus_reasons.push_back("mean_reversion_setup");
        } else if (features.volatility.has_impulse &&
                   features.trend_quality.body_to_range_ratio > 0.6) {
            s.regime_tag = RegimeTag::Breakout;
            s.bonus_reasons.push_back("breakout_setup");
        } else {
            s.regime_tag = RegimeTag::Avoid;
            s.penalty_reasons.push_back("no_clear_regime");
        }

        // Component breakdown в bonus/penalty reasons
        if (spread_comp > 0.7) s.bonus_reasons.push_back("scalp_tight_spread");
        if (depth_comp > 0.7) s.bonus_reasons.push_back("scalp_deep_book");
        if (resiliency_comp > 0.6) s.bonus_reasons.push_back("scalp_resilient_book");
        if (vol_quality_comp > 0.7) s.bonus_reasons.push_back("scalp_vol_sweet_spot");
        if (trade_flow_comp > 0.5) s.bonus_reasons.push_back("scalp_strong_flow");
        if (micro_comp > 0.5) s.bonus_reasons.push_back("scalp_clean_candles");
        if (exec_q_comp > 0.7) s.bonus_reasons.push_back("scalp_low_slippage");
    }

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
