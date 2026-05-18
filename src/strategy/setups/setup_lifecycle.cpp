#include "strategy/setups/setup_lifecycle.hpp"
#include "indicators/advanced_indicators.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace tb::strategy {

namespace {

/// Bug 4.1 fix: extracted Bayesian fusion helper для использования всеми detect_*.
/// Анализирует 10 advanced indicators через likelihood ratios → posterior P(bullish/bearish).
/// Возвращает результат Bayesian fusion + полное число signals для caller'а.
/// caller сам решает reject/accept based on его direction.
struct BayesianFilterResult {
    double p_bullish{0.5};
    double p_bearish{0.5};
    double confidence{0.0};
    int signals_count{0};
};

[[nodiscard]] BayesianFilterResult run_bayesian_indicator_filter(
    const features::TechnicalFeatures& tech,
    double mid_price)
{
    std::vector<indicators::SignalLikelihood> bayes_signals;
    auto push_signal = [&](double lr_bullish, double lr_bearish) {
        if (lr_bullish > 0.0 && lr_bearish > 0.0
            && std::isfinite(lr_bullish) && std::isfinite(lr_bearish)) {
            bayes_signals.push_back({lr_bullish, lr_bearish});
        }
    };

    // 1. Supertrend trend — hard primary filter.
    if (tech.supertrend_valid) {
        if (tech.supertrend_trend == 1)       push_signal(2.0, 0.5);
        else if (tech.supertrend_trend == -1) push_signal(0.5, 2.0);
    }
    // 2. EMA pair (9/21).
    if (tech.ema_pair_valid) {
        if (tech.ema_pair_trend == 1)        push_signal(1.7, 0.6);
        else if (tech.ema_pair_trend == -1)  push_signal(0.6, 1.7);
    }
    // 3. Stochastic — overbought/oversold + crossovers.
    if (tech.stoch_valid) {
        if (tech.stoch_oversold)              push_signal(1.5, 0.7);
        else if (tech.stoch_overbought)       push_signal(0.7, 1.5);
        if (tech.stoch_bull_cross)            push_signal(1.8, 0.6);
        if (tech.stoch_bear_cross)            push_signal(0.6, 1.8);
    }
    // 4. Anchored VWAP — bias + 2σ extremes (mean revert).
    if (tech.avwap_valid) {
        if (tech.avwap_price_vs_vwap_bps > 0.0)      push_signal(1.4, 0.7);
        else if (tech.avwap_price_vs_vwap_bps < 0.0) push_signal(0.7, 1.4);
        if (mid_price > tech.avwap_upper_2sigma)     push_signal(0.5, 1.8);
        else if (mid_price < tech.avwap_lower_2sigma) push_signal(1.8, 0.5);
    }
    // 5. CVD — taker pressure + divergences.
    if (tech.cvd_valid) {
        if (tech.cvd_normalized > 0.2)             push_signal(1.6, 0.6);
        else if (tech.cvd_normalized < -0.2)       push_signal(0.6, 1.6);
        if (tech.cvd_bullish_divergence)           push_signal(2.0, 0.5);
        if (tech.cvd_bearish_divergence)           push_signal(0.5, 2.0);
    }
    // 6. Open Interest 4-quadrant Wyckoff.
    if (tech.oi_valid) {
        switch (tech.oi_trend_quadrant) {
            case 1: push_signal(1.5, 0.7); break;
            case 2: push_signal(0.7, 1.5); break;
            case 3: push_signal(0.9, 1.2); break;
            case 4: push_signal(1.2, 0.9); break;
            default: break;
        }
    }
    // 7. Liquidity sweep — fade wick direction.
    if (tech.liq_sweep_valid) {
        if (tech.liq_sweep_high) push_signal(0.4, 2.0);
        if (tech.liq_sweep_low)  push_signal(2.0, 0.4);
    }
    // 8. Funding bias mean revert.
    if (tech.funding_valid && tech.funding_crowding_intensity > 0.5) {
        if (tech.funding_recommended_bias == 1)        push_signal(1.4, 0.7);
        else if (tech.funding_recommended_bias == -1)  push_signal(0.7, 1.4);
    }
    // 9. Spoof detection — degrade both directions.
    if (tech.spoof_valid && tech.spoof_intensity > 0.7) {
        push_signal(0.85, 0.85);
    }
    // 10. Liquidation cascade risk.
    if (tech.liq_valid && tech.liq_cascade_risk_score > 0.7) {
        if (tech.liq_dominant_side == 1)       push_signal(0.6, 1.3);
        else if (tech.liq_dominant_side == -1) push_signal(1.3, 0.6);
    }

    auto bayes = indicators::combine_signals_bayesian(bayes_signals, 0.5);
    BayesianFilterResult r;
    r.p_bullish = bayes.p_bullish;
    r.p_bearish = bayes.p_bearish;
    r.confidence = bayes.confidence;
    r.signals_count = static_cast<int>(bayes_signals.size());
    return r;
}

}  // namespace


// ═══════════════════════════════════════════════════════════════════════════════
// SetupDetector
// ═══════════════════════════════════════════════════════════════════════════════

std::optional<Setup> SetupDetector::detect(const StrategyContext& ctx,
                                           const std::string& setup_id,
                                           int64_t now_ns) const {
    // Gating по world state и uncertainty:
    // - Экстремальная неопределённость → не генерируем сетапы
    // - Disrupted world state → не генерируем сетапы (токсичный, вакуум, кризис)
    if (ctx.uncertainty == UncertaintyLevel::Extreme) return std::nullopt;
    if (ctx.world_state == WorldStateLabel::Disrupted) return std::nullopt;

    // High uncertainty → только retest и rejection (более консервативные сетапы)
    bool conservative_only = (ctx.uncertainty == UncertaintyLevel::High);

    // Ranging regime → блокируем momentum и pullback
    bool chop_mode = (ctx.regime == RegimeLabel::Ranging);

    // Пробуем каждый тип сетапа в порядке приоритета
    if (cfg_.enable_momentum_continuation && !chop_mode && !conservative_only) {
        if (auto s = detect_momentum(ctx, setup_id, now_ns)) return s;
    }
    if (cfg_.enable_retest_scenarios) {
        if (auto s = detect_retest(ctx, setup_id, now_ns)) return s;
    }
    if (cfg_.enable_pullback_scenarios && !chop_mode && !conservative_only) {
        if (auto s = detect_pullback(ctx, setup_id, now_ns)) return s;
    }
    if (cfg_.enable_rejection_scenarios) {
        if (auto s = detect_rejection(ctx, setup_id, now_ns)) return s;
    }
    return std::nullopt;
}

std::optional<Setup> SetupDetector::detect_momentum(const StrategyContext& ctx,
                                                     const std::string& id,
                                                     int64_t now_ns) const {
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    if (!tech.adx_valid || tech.adx < cfg_.adx_min) return std::nullopt;
    if (!tech.momentum_valid) return std::nullopt;
    // Bug 9.1 fix: hard guard на ATR. Без verified ATR расчёт SL/TP unsafe.
    // Раньше fallback `mid * 0.005` давал 50 bps SL — слишком tight для high-vol,
    // слишком wide для stable pairs. Симметрично с другими detect_* которые имеют guard.
    if (!tech.atr_valid || !std::isfinite(tech.atr_14) || tech.atr_14 <= 0.0) {
        return std::nullopt;
    }

    double imb = micro.book_imbalance_5;
    double bsr = micro.buy_sell_ratio;
    double mid = micro.mid_price;
    if (mid <= 0.0) return std::nullopt;
    if (!std::isfinite(imb) || !std::isfinite(bsr)) return std::nullopt;

    // BUG-EDGE-7 (live run18 SAHARA): mom5 alignment теперь HARD GATE, не optional score.
    // Старый score 2/3 пропускал anti-momentum entries: SAHARA SELL @ mom5=+0.018 (BULLISH)
    // потому что imb+bsr давали 2 точки. Теперь mom5 ОБЯЗАТЕЛЕН для side direction.
    bool mom5_buy_aligned  = (tech.momentum_5 > cfg_.momentum_min_buy);
    bool mom5_sell_aligned = (tech.momentum_5 < cfg_.momentum_max_sell);

    // BUY: mom5 align + 1 из 2 microstructure подтверждений
    int buy_score = (imb > cfg_.imbalance_threshold ? 1 : 0)
                  + (bsr > cfg_.buy_sell_ratio_buy ? 1 : 0);
    bool buy_signal = mom5_buy_aligned && (buy_score >= 1) &&
                      (bsr > 1.0 || imb > cfg_.imbalance_threshold);

    // SELL: mom5 align + 1 из 2
    int sell_score = (imb < -cfg_.imbalance_threshold ? 1 : 0)
                   + (bsr < cfg_.buy_sell_ratio_sell ? 1 : 0);
    bool sell_signal = mom5_sell_aligned && (sell_score >= 1) &&
                       (bsr < 1.0 || imb < -cfg_.imbalance_threshold);

    // EDGE-14: re-enable SELL с STRICT momentum threshold (mom_max_sell=-0.25% via EDGE-14).
    // Previous shorts had mom5 в [-0.08%, -0.5%] — weak edge. Сильный момент только при
    // mom5 < -0.25%. Sample collection начнётся заново.

    if (!buy_signal && !sell_signal) return std::nullopt;

    // Bug 2.2 fix: Adaptive RSI thresholds — wider при high vol, tighter при low vol.
    // B22.1 fix: baseline_vol теперь из config.
    auto adapt = indicators::compute_adaptive_thresholds(
        tech.volatility_valid ? tech.volatility_5 : cfg_.adaptive_baseline_vol,
        cfg_.adaptive_baseline_vol);
    if (buy_signal && tech.rsi_valid && tech.rsi_14 > adapt.rsi_overbought) return std::nullopt;
    if (sell_signal && tech.rsi_valid && tech.rsi_14 < adapt.rsi_oversold) return std::nullopt;

    // BB guards
    if (buy_signal && tech.bb_valid && tech.bb_percent_b > cfg_.bb_max_buy) return std::nullopt;
    if (sell_signal && tech.bb_valid && tech.bb_percent_b < cfg_.bb_min_sell) return std::nullopt;

    // Тренд guard (1m EMA)
    if (cfg_.block_counter_trend && tech.ema_valid) {
        if (buy_signal && tech.ema_20 < tech.ema_50) return std::nullopt;
        if (sell_signal && tech.ema_20 > tech.ema_50) return std::nullopt;
    }

    // HTF тренд guard: блокируем вход против СИЛЬНОГО тренда старшего ТФ
    // run95 fix: 0.7 → 0.5 — жёстче блокируем counter-trend для уменьшения wrong-direction sigals
    if (cfg_.block_counter_trend && std::isfinite(ctx.htf_trend_strength) && ctx.htf_trend_strength > 0.5) {
        // BUY при очень сильном HTF DOWN — запрещено
        if (buy_signal && ctx.htf_trend_direction == -1) return std::nullopt;
        // SELL при очень сильном HTF UP — запрещено
        if (sell_signal && ctx.htf_trend_direction == 1) return std::nullopt;
    }
    // При SIDEWAYS HTF — требуем чуть более строгий ADX на рабочем ТФ.
    // B22.6 fix: boost из config.
    if (cfg_.block_counter_trend && ctx.htf_trend_direction == 0 &&
        tech.adx < cfg_.adx_min + cfg_.adx_sideways_boost) {
        return std::nullopt;
    }

    // Short entry только при futures
    if (sell_signal && !ctx.futures_enabled) {
        // На споте SELL = только закрытие long. Допускаем если есть позиция
        if (!ctx.position.has_position) return std::nullopt;
    }

    // Bug 4.1 fix: Bayesian fusion 10 advanced indicators через helper.
    // B22.4 fix: threshold из config.
    auto bayes = run_bayesian_indicator_filter(tech, mid);
    if (buy_signal && bayes.p_bullish < cfg_.bayesian_min_confidence) return std::nullopt;
    if (sell_signal && bayes.p_bearish < cfg_.bayesian_min_confidence) return std::nullopt;

    Side side = buy_signal ? Side::Buy : Side::Sell;

    // Confidence: combine base imbalance strength + Bayesian fusion confidence.
    // B22.2/B22.3 fix: divisor и weights из config.
    double imb_strength = std::min(std::abs(imb) / cfg_.imbalance_strength_divisor, 1.0);
    double spread_factor = std::clamp(1.0 - micro.spread_bps / cfg_.max_spread_bps_for_entry, 0.0, 1.0);
    double confidence = cfg_.base_conviction
        + imb_strength * spread_factor * cfg_.imb_strength_weight
        + bayes.confidence * cfg_.bayesian_confidence_weight;
    if (tech.ema_valid) {
        bool trend_aligned = buy_signal ? (tech.ema_20 > tech.ema_50) : (tech.ema_20 < tech.ema_50);
        if (trend_aligned) confidence += cfg_.trend_bonus;
    }

    // Volume Profile: цена у POC = высоколиквидная зона.
    // B22.5 fix: POC distances/bonuses теперь из config.
    if (tech.vp_valid && mid > 0.0) {
        double poc_dist = std::abs(tech.vp_price_vs_poc);
        if (poc_dist < cfg_.poc_close_distance) {
            confidence += cfg_.poc_close_bonus;
        } else if (poc_dist > cfg_.poc_far_distance) {
            confidence -= cfg_.poc_far_penalty;
        }
    }

    confidence = std::min(confidence, cfg_.max_conviction);

    // Стоп = ATR-based, TP = R:R-симметричный (см. edge-31 TPSL refactor).
    // Bug 9.1 fix: ATR guarded в начале функции — здесь tech.atr_14 always valid.
    double atr = tech.atr_14;
    double stop = buy_signal ? mid - atr * cfg_.atr_stop_mult_momentum
                              : mid + atr * cfg_.atr_stop_mult_momentum;
    double tp   = buy_signal ? mid + atr * cfg_.atr_target_mult_momentum
                              : mid - atr * cfg_.atr_target_mult_momentum;

    Setup setup;
    setup.id = id;
    setup.type = SetupType::MomentumContinuation;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
    setup.tp_reference = tp;
    setup.entry_reference = mid;
    setup.detected_at_ns = now_ns;
    setup.last_check_ns = now_ns;
    setup.spread_bps_at_detect = micro.spread_bps;
    setup.imbalance_at_detect = imb;
    setup.atr_at_detect = atr;
    setup.rsi_at_detect = tech.rsi_valid ? tech.rsi_14 : 50.0;
    setup.reasons.push_back("momentum_continuation");
    setup.reasons.push_back(buy_signal ? "buy_imbalance_strong" : "sell_imbalance_strong");
    setup.reasons.push_back("adx_trend_confirmed");
    if (tech.ema_valid && ((buy_signal && tech.ema_20 > tech.ema_50) || (sell_signal && tech.ema_20 < tech.ema_50))) {
        setup.reasons.push_back("ema_trend_aligned");
    }

    return setup;
}

std::optional<Setup> SetupDetector::detect_retest(const StrategyContext& ctx,
                                                   const std::string& id,
                                                   int64_t now_ns) const {
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    if (!tech.bb_valid || !tech.momentum_valid || !tech.atr_valid) return std::nullopt;

    double mid = micro.mid_price;
    if (mid <= 0.0) return std::nullopt;

    // Ретест: цена вернулась к недавнему уровню после пробоя
    // Используем Bollinger Bands как ориентир: цена была за BB, вернулась к middle
    double atr = tech.atr_14;
    double tolerance = mid * cfg_.retest_level_tolerance_pct;

    // Proximity check: цена должна быть достаточно близко к BB middle (уровень ретеста)
    if (tech.bb_middle > 0.0 && std::abs(mid - tech.bb_middle) > tolerance) return std::nullopt;

    // BUY retest: цена у BB lower-middle, momentum_20 > 0 (общий тренд вверх)
    // H-12 fix: non-overlapping BB%B ranges (was 0.35-0.55 / 0.45-0.65 → overlap 0.45-0.55)
    bool buy_retest = (tech.bb_percent_b > 0.30 && tech.bb_percent_b < 0.48) &&
                      (tech.momentum_20 > 0.0) &&
                      (micro.book_imbalance_5 > 0.05);

    // SELL retest: цена у BB upper-middle, momentum_20 < 0
    bool sell_retest = (tech.bb_percent_b > 0.52 && tech.bb_percent_b < 0.70) &&
                       (tech.momentum_20 < 0.0) &&
                       (micro.book_imbalance_5 < -0.05);

    if (!buy_retest && !sell_retest) return std::nullopt;

    // Тренд guard (1m EMA)
    if (cfg_.block_counter_trend && tech.ema_valid) {
        if (buy_retest && tech.ema_20 < tech.ema_50) return std::nullopt;
        if (sell_retest && tech.ema_20 > tech.ema_50) return std::nullopt;
    }

    // HTF тренд guard для retest: только при очень сильном тренде
    // run95 fix: 0.7 → 0.5 — жёстче блокируем counter-trend для уменьшения wrong-direction sigals
    if (cfg_.block_counter_trend && std::isfinite(ctx.htf_trend_strength) && ctx.htf_trend_strength > 0.5) {
        if (buy_retest && ctx.htf_trend_direction == -1) return std::nullopt;
        if (sell_retest && ctx.htf_trend_direction == 1) return std::nullopt;
    }
    // SIDEWAYS: retest ненадёжен без сильного тренда на 1m
    if (cfg_.block_counter_trend && ctx.htf_trend_direction == 0 && tech.adx < cfg_.adx_min + 3.0) {
        return std::nullopt;
    }

    if (sell_retest && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    // RSI guards
    if (buy_retest && tech.rsi_valid && tech.rsi_14 > cfg_.rsi_upper_guard) return std::nullopt;
    if (sell_retest && tech.rsi_valid && tech.rsi_14 < cfg_.rsi_lower_guard) return std::nullopt;

    // Bug 4.1 fix: Bayesian fusion 10 advanced indicators.
    {
        auto bayes = run_bayesian_indicator_filter(tech, mid);
        if (buy_retest && bayes.p_bullish < 0.55) return std::nullopt;
        if (sell_retest && bayes.p_bearish < 0.55) return std::nullopt;
    }

    Side side = buy_retest ? Side::Buy : Side::Sell;

    double confidence = cfg_.base_conviction + 0.05;  // Ретест = умеренная уверенность
    if (tech.ema_valid) {
        bool trend_aligned = buy_retest ? (tech.ema_20 > tech.ema_50) : (tech.ema_20 < tech.ema_50);
        if (trend_aligned) confidence += cfg_.trend_bonus;
    }
    confidence = std::min(confidence, cfg_.max_conviction);

    double stop = buy_retest ? mid - atr * cfg_.atr_stop_mult_retest
                              : mid + atr * cfg_.atr_stop_mult_retest;
    double tp   = buy_retest ? mid + atr * cfg_.atr_target_mult_retest
                              : mid - atr * cfg_.atr_target_mult_retest;

    Setup setup;
    setup.id = id;
    setup.type = SetupType::Retest;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
    setup.tp_reference = tp;
    setup.entry_reference = mid;
    setup.detected_at_ns = now_ns;
    setup.last_check_ns = now_ns;
    setup.spread_bps_at_detect = micro.spread_bps;
    setup.imbalance_at_detect = micro.book_imbalance_5;
    setup.atr_at_detect = atr;
    setup.rsi_at_detect = tech.rsi_valid ? tech.rsi_14 : 50.0;
    setup.reasons.push_back("retest_level_holding");
    setup.reasons.push_back(buy_retest ? "price_at_support_after_breakout" : "price_at_resistance_after_breakdown");

    return setup;
}

std::optional<Setup> SetupDetector::detect_pullback(const StrategyContext& ctx,
                                                     const std::string& id,
                                                     int64_t now_ns) const {
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    if (!tech.adx_valid || !tech.momentum_valid || !tech.ema_valid || !tech.atr_valid) {
        return std::nullopt;
    }

    double mid = micro.mid_price;
    if (mid <= 0.0) return std::nullopt;

    // Пулбэк: сильный тренд + откат против тренда
    // ADX > threshold = сильный тренд
    if (tech.adx < cfg_.adx_min + 5.0) return std::nullopt;  // Нужен тренд сильнее обычного

    bool uptrend = tech.ema_20 > tech.ema_50;
    bool downtrend = tech.ema_20 < tech.ema_50;

    // BUY pullback: аптренд + краткосрочный откат (momentum_5 < 0 при momentum_20 > 0)
    bool buy_pullback = uptrend &&
                        (tech.momentum_20 > 0.0) &&
                        (tech.momentum_5 < 0.0) &&
                        (micro.book_imbalance_5 > -0.1);  // Стакан не против

    // SELL pullback: даунтренд + краткосрочный отскок
    bool sell_pullback = downtrend &&
                         (tech.momentum_20 < 0.0) &&
                         (tech.momentum_5 > 0.0) &&
                         (micro.book_imbalance_5 < 0.1);

    if (!buy_pullback && !sell_pullback) return std::nullopt;

    // HTF тренд guard для pullback: только при очень сильном тренде
    // run95 fix: 0.7 → 0.5 — жёстче блокируем counter-trend для уменьшения wrong-direction sigals
    if (cfg_.block_counter_trend && std::isfinite(ctx.htf_trend_strength) && ctx.htf_trend_strength > 0.5) {
        if (buy_pullback && ctx.htf_trend_direction == -1) return std::nullopt;
        if (sell_pullback && ctx.htf_trend_direction == 1) return std::nullopt;
    }

    // BUG-EDGE-2 (live run11): require deeper pullback. Без этого strategy fires при
    // RSI 43-51 — shallow pullback с unconfirmed reversal. Live observed: 3 losses подряд
    // на ASTERUSDT BUY pullback с RSI 43-51, цена продолжала падать после entry.
    // Теперь BUY требует RSI ≤ 45 (real oversold zone), SELL требует RSI ≥ 55.
    if (buy_pullback && tech.rsi_valid && tech.rsi_14 > cfg_.pullback_rsi_buy_max) return std::nullopt;
    if (sell_pullback && tech.rsi_valid && tech.rsi_14 < cfg_.pullback_rsi_sell_min) return std::nullopt;

    if (sell_pullback && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    // Bug 4.1 fix: Bayesian fusion 10 advanced indicators.
    {
        auto bayes = run_bayesian_indicator_filter(tech, mid);
        if (buy_pullback && bayes.p_bullish < 0.55) return std::nullopt;
        if (sell_pullback && bayes.p_bearish < 0.55) return std::nullopt;
    }

    Side side = buy_pullback ? Side::Buy : Side::Sell;
    double atr = tech.atr_14;
    double stop = buy_pullback ? mid - atr * cfg_.atr_stop_mult_pullback
                                : mid + atr * cfg_.atr_stop_mult_pullback;
    double tp   = buy_pullback ? mid + atr * cfg_.atr_target_mult_pullback
                                : mid - atr * cfg_.atr_target_mult_pullback;

    double confidence = cfg_.base_conviction + 0.08;  // Пулбэк в тренде = хорошая уверенность
    confidence += cfg_.trend_bonus;  // Тренд всегда подтверждён для pullback
    confidence = std::min(confidence, cfg_.max_conviction);

    Setup setup;
    setup.id = id;
    setup.type = SetupType::PullbackInMicrotrend;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
    setup.tp_reference = tp;
    setup.entry_reference = mid;
    setup.detected_at_ns = now_ns;
    setup.last_check_ns = now_ns;
    setup.spread_bps_at_detect = micro.spread_bps;
    setup.imbalance_at_detect = micro.book_imbalance_5;
    setup.atr_at_detect = atr;
    setup.rsi_at_detect = tech.rsi_valid ? tech.rsi_14 : 50.0;
    setup.reasons.push_back("pullback_in_microtrend");
    setup.reasons.push_back(buy_pullback ? "uptrend_pullback" : "downtrend_pullback");
    setup.reasons.push_back("adx_strong_trend");

    return setup;
}

std::optional<Setup> SetupDetector::detect_rejection(const StrategyContext& ctx,
                                                      const std::string& id,
                                                      int64_t now_ns) const {
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    if (!tech.bb_valid || !tech.momentum_valid || !tech.atr_valid) return std::nullopt;

    double mid = micro.mid_price;
    if (mid <= 0.0) return std::nullopt;

    // Rejection: цена у экстремального уровня BB с признаками отбоя
    // BUY rejection: цена у нижней BB + разворот вверх
    bool buy_rejection = (tech.bb_percent_b < (1.0 - cfg_.rejection_bb_extreme)) &&
                         (tech.momentum_5 > cfg_.rejection_min_reversal_momentum) &&
                         (micro.book_imbalance_5 > cfg_.imbalance_threshold * 0.5);

    // SELL rejection: цена у верхней BB + разворот вниз
    bool sell_rejection = (tech.bb_percent_b > cfg_.rejection_bb_extreme) &&
                          (tech.momentum_5 < -cfg_.rejection_min_reversal_momentum) &&
                          (micro.book_imbalance_5 < -cfg_.imbalance_threshold * 0.5);

    if (!buy_rejection && !sell_rejection) return std::nullopt;

    // RSI подтверждение — M-16 fix: Wilder standard oversold/overbought zones
    // Buy rejection: RSI must be in oversold territory (< 35), not just < 50
    // Sell rejection: RSI must be in overbought territory (> 65), not just > 50
    if (buy_rejection && tech.rsi_valid && tech.rsi_14 > 35.0) return std::nullopt;
    if (sell_rejection && tech.rsi_valid && tech.rsi_14 < 65.0) return std::nullopt;

    // Bug 4.1 fix: Bayesian fusion 10 advanced indicators.
    {
        auto bayes = run_bayesian_indicator_filter(tech, mid);
        if (buy_rejection && bayes.p_bullish < 0.55) return std::nullopt;
        if (sell_rejection && bayes.p_bearish < 0.55) return std::nullopt;
    }

    if (sell_rejection && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    Side side = buy_rejection ? Side::Buy : Side::Sell;
    double atr = tech.atr_14;
    double stop = buy_rejection ? mid - atr * cfg_.atr_stop_mult_rejection
                                 : mid + atr * cfg_.atr_stop_mult_rejection;
    double tp   = buy_rejection ? mid + atr * cfg_.atr_target_mult_rejection
                                 : mid - atr * cfg_.atr_target_mult_rejection;

    // Rejection = более низкая уверенность (контр-трендовый)
    double confidence = cfg_.base_conviction - 0.02;
    confidence = std::min(std::max(confidence, 0.1), cfg_.max_conviction);

    Setup setup;
    setup.id = id;
    setup.type = SetupType::Rejection;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
    setup.tp_reference = tp;
    setup.entry_reference = mid;
    setup.detected_at_ns = now_ns;
    setup.last_check_ns = now_ns;
    setup.spread_bps_at_detect = micro.spread_bps;
    setup.imbalance_at_detect = micro.book_imbalance_5;
    setup.atr_at_detect = atr;
    setup.rsi_at_detect = tech.rsi_valid ? tech.rsi_14 : 50.0;
    setup.reasons.push_back("rejection_from_extreme");
    setup.reasons.push_back(buy_rejection ? "bb_lower_rejection" : "bb_upper_rejection");

    return setup;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SetupValidator
// ═══════════════════════════════════════════════════════════════════════════════

SetupValidationResult SetupValidator::validate(const Setup& setup,
                                               const StrategyContext& ctx,
                                               int64_t now_ns) const {
    SetupValidationResult result;
    result.valid = true;
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    // 1. Таймаут сетапа (§15)
    // BUG-S35-05: negative age_ms when NTP backward jump → timeout never fires.
    // Treat negative age as 0 — setup just detected, conservative.
    int64_t age_ms = std::max(int64_t{0}, now_ns - setup.detected_at_ns) / 1'000'000;
    if (age_ms > cfg_.setup_timeout_ms) {
        result.valid = false;
        result.reasons.push_back("setup_timeout");
        return result;
    }

    // 2. Спред ухудшился
    if (micro.spread_valid && micro.spread_bps > cfg_.max_spread_bps_for_entry * 1.5) {
        result.valid = false;
        result.conditions_degraded = true;
        result.reasons.push_back("spread_degraded");
        return result;
    }

    // 3. Ликвидность ухудшилась — soft penalty, не hard reject
    // В books15 (15 уровней) перекосы bid/ask частые и кратковременные
    if (micro.liquidity_valid && micro.liquidity_ratio < cfg_.min_liquidity_ratio * 0.2) {
        // Только при крайне низкой ликвидности (ratio < 0.06) — hard reject
        result.valid = false;
        result.conditions_degraded = true;
        result.reasons.push_back("liquidity_degraded");
        return result;
    }
    if (micro.liquidity_valid && micro.liquidity_ratio < cfg_.min_liquidity_ratio) {
        // Мягкий штраф — помечаем деградацию, но не отвергаем
        result.conditions_degraded = true;
        result.reasons.push_back("liquidity_warning");
    }

    // 4. VPIN повышен — мягкий штраф, а не hard reject
    //    Регим ToxicFlow уже снижает веса стратегий (0.1-0.3×),
    //    аллокацию (10% cap) и risk-лимиты — этого достаточно.
    if (micro.vpin_valid && micro.vpin_toxic) {
        result.conditions_degraded = true;
        result.reasons.push_back("vpin_elevated");
    }

    // 5. Структура сломалась — imbalance развернулся сильно
    //    M-17 fix: 10× was unrealistic (requires 80% of book to reverse).
    //    Use 3× = 0.24 which is achievable in volatile crypto books.
    bool imbalance_reversed = false;
    const double rev_threshold = cfg_.imbalance_threshold * 3.0;  // 3× — meaningful but achievable
    if (setup.side == Side::Buy && micro.book_imbalance_5 < -rev_threshold) {
        imbalance_reversed = true;
    }
    if (setup.side == Side::Sell && micro.book_imbalance_5 > rev_threshold) {
        imbalance_reversed = true;
    }
    if (imbalance_reversed) {
        result.valid = false;
        result.reasons.push_back("imbalance_reversed");
        return result;
    }

    // 6. Цена сильно ушла от reference (слишком далеко для входа)
    if (tech.atr_valid && std::isfinite(tech.atr_14) && tech.atr_14 > 0.0) {
        double price_move = std::abs(micro.mid_price - setup.reference_price);
        if (price_move > tech.atr_14 * 2.0) {
            result.valid = false;
            result.reasons.push_back("price_moved_too_far");
            return result;
        }
    }

    // 7. Уровень стопа пробит (invalidation)
    if (setup.side == Side::Buy && micro.mid_price < setup.stop_reference) {
        result.valid = false;
        result.reasons.push_back("stop_level_breached");
        return result;
    }
    if (setup.side == Side::Sell && micro.mid_price > setup.stop_reference) {
        result.valid = false;
        result.reasons.push_back("stop_level_breached");
        return result;
    }

    result.reasons.push_back("setup_still_valid");
    return result;
}

bool SetupValidator::can_confirm(const Setup& setup,
                                 const StrategyContext& ctx,
                                 int64_t now_ns) const {
    const auto& micro = ctx.features.microstructure;

    // EDGE-22 (architectural fix 2026-05-14): для HIGH-CONFIDENCE setups (conf > 0.85)
    // пропускаем confirmation window. Это устраняет latency 800ms для best setups.
    // Сейчас 80% setups погибают в confirmation window от imbalance_reversed.
    // Скальпинг — каждые 800ms цена двигается. High-conv signals не должны ждать.
    bool high_confidence = setup.confidence > 0.85;

    if (!high_confidence) {
        // Подтверждение: прошло достаточно времени + структура держится
        // BUG-S35-05: clamp negative age to 0 on NTP backward jump
        int64_t age_ms = std::max(int64_t{0}, now_ns - setup.detected_at_ns) / 1'000'000;
        if (age_ms < cfg_.setup_confirmation_window_ms) {
            return false;
        }
    }

    // Спред по-прежнему в пределах нормы
    if (micro.spread_valid && micro.spread_bps > cfg_.max_spread_bps_for_entry) {
        return false;
    }

    // Imbalance всё ещё в нужную сторону
    if (setup.side == Side::Buy && micro.book_imbalance_5 < 0.0) {
        return false;
    }
    if (setup.side == Side::Sell && micro.book_imbalance_5 > 0.0) {
        return false;
    }

    return true;
}

} // namespace tb::strategy
