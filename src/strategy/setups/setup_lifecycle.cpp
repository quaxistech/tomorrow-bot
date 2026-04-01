#include "strategy/setups/setup_lifecycle.hpp"
#include <cmath>
#include <algorithm>

namespace tb::strategy {

// ═══════════════════════════════════════════════════════════════════════════════
// SetupDetector
// ═══════════════════════════════════════════════════════════════════════════════

std::optional<Setup> SetupDetector::detect(const StrategyContext& ctx,
                                           const std::string& setup_id,
                                           int64_t now_ns) const {
    // Пробуем каждый тип сетапа в порядке приоритета
    if (cfg_.enable_momentum_continuation) {
        if (auto s = detect_momentum(ctx, setup_id, now_ns)) return s;
    }
    if (cfg_.enable_retest_scenarios) {
        if (auto s = detect_retest(ctx, setup_id, now_ns)) return s;
    }
    if (cfg_.enable_pullback_scenarios) {
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

    double imb = micro.book_imbalance_5;
    double bsr = micro.buy_sell_ratio;
    double mid = micro.mid_price;
    if (mid <= 0.0) return std::nullopt;

    // BUY: сильный перекос стакана + покупатели доминируют + импульс вверх
    bool buy_signal = (imb > cfg_.imbalance_threshold) &&
                      (bsr > cfg_.buy_sell_ratio_buy) &&
                      (tech.momentum_5 > cfg_.momentum_min_buy);

    // SELL: обратный перекос + продавцы доминируют + импульс вниз
    bool sell_signal = (imb < -cfg_.imbalance_threshold) &&
                       (bsr < cfg_.buy_sell_ratio_sell) &&
                       (tech.momentum_5 < cfg_.momentum_max_sell);

    if (!buy_signal && !sell_signal) return std::nullopt;

    // RSI guards
    if (buy_signal && tech.rsi_valid && tech.rsi_14 > cfg_.rsi_upper_guard) return std::nullopt;
    if (sell_signal && tech.rsi_valid && tech.rsi_14 < cfg_.rsi_lower_guard) return std::nullopt;

    // BB guards
    if (buy_signal && tech.bb_valid && tech.bb_percent_b > cfg_.bb_max_buy) return std::nullopt;
    if (sell_signal && tech.bb_valid && tech.bb_percent_b < cfg_.bb_min_sell) return std::nullopt;

    // Тренд guard
    if (cfg_.block_counter_trend && tech.ema_valid) {
        if (buy_signal && tech.ema_20 < tech.ema_50) return std::nullopt;
        if (sell_signal && tech.ema_20 > tech.ema_50) return std::nullopt;
    }

    // Short entry только при futures
    if (sell_signal && !ctx.futures_enabled) {
        // На споте SELL = только закрытие long. Допускаем если есть позиция
        if (!ctx.position.has_position) return std::nullopt;
    }

    Side side = buy_signal ? Side::Buy : Side::Sell;

    // Расчёт confidence
    double imb_strength = std::min(std::abs(imb) / 0.5, 1.0);
    double spread_factor = std::clamp(1.0 - micro.spread_bps / cfg_.max_spread_bps_for_entry, 0.0, 1.0);
    double confidence = cfg_.base_conviction + imb_strength * spread_factor * 0.3;
    if (tech.ema_valid) {
        bool trend_aligned = buy_signal ? (tech.ema_20 > tech.ema_50) : (tech.ema_20 < tech.ema_50);
        if (trend_aligned) confidence += cfg_.trend_bonus;
    }
    confidence = std::min(confidence, cfg_.max_conviction);

    // Стоп = ATR-based
    double atr = tech.atr_valid ? tech.atr_14 : mid * 0.005;
    double stop = buy_signal ? mid - atr * 2.0 : mid + atr * 2.0;

    Setup setup;
    setup.id = id;
    setup.type = SetupType::MomentumContinuation;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
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

    // BUY retest: цена у BB middle снизу, momentum_20 > 0 (общий тренд вверх)
    bool buy_retest = (tech.bb_percent_b > 0.35 && tech.bb_percent_b < 0.55) &&
                      (tech.momentum_20 > 0.0) &&
                      (micro.book_imbalance_5 > 0.05);

    // SELL retest: цена у BB middle сверху, momentum_20 < 0
    bool sell_retest = (tech.bb_percent_b > 0.45 && tech.bb_percent_b < 0.65) &&
                       (tech.momentum_20 < 0.0) &&
                       (micro.book_imbalance_5 < -0.05);

    if (!buy_retest && !sell_retest) return std::nullopt;

    // Тренд guard
    if (cfg_.block_counter_trend && tech.ema_valid) {
        if (buy_retest && tech.ema_20 < tech.ema_50) return std::nullopt;
        if (sell_retest && tech.ema_20 > tech.ema_50) return std::nullopt;
    }

    if (sell_retest && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    // RSI guards
    if (buy_retest && tech.rsi_valid && tech.rsi_14 > cfg_.rsi_upper_guard) return std::nullopt;
    if (sell_retest && tech.rsi_valid && tech.rsi_14 < cfg_.rsi_lower_guard) return std::nullopt;

    Side side = buy_retest ? Side::Buy : Side::Sell;

    double confidence = cfg_.base_conviction + 0.05;  // Ретест = умеренная уверенность
    if (tech.ema_valid) {
        bool trend_aligned = buy_retest ? (tech.ema_20 > tech.ema_50) : (tech.ema_20 < tech.ema_50);
        if (trend_aligned) confidence += cfg_.trend_bonus;
    }
    confidence = std::min(confidence, cfg_.max_conviction);

    double stop = buy_retest ? mid - atr * 2.5 : mid + atr * 2.5;

    Setup setup;
    setup.id = id;
    setup.type = SetupType::Retest;
    setup.side = side;
    setup.confidence = confidence;
    setup.reference_price = mid;
    setup.stop_reference = stop;
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

    // RSI: не должен быть в экстремальной зоне
    if (buy_pullback && tech.rsi_valid && tech.rsi_14 > cfg_.rsi_upper_guard) return std::nullopt;
    if (sell_pullback && tech.rsi_valid && tech.rsi_14 < cfg_.rsi_lower_guard) return std::nullopt;

    if (sell_pullback && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    Side side = buy_pullback ? Side::Buy : Side::Sell;
    double atr = tech.atr_14;
    double stop = buy_pullback ? mid - atr * 2.0 : mid + atr * 2.0;

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

    // RSI дополнительное подтверждение — должен быть в экстремальной зоне (подтверждает отбой)
    if (buy_rejection && tech.rsi_valid && tech.rsi_14 > 50.0) return std::nullopt;
    if (sell_rejection && tech.rsi_valid && tech.rsi_14 < 50.0) return std::nullopt;

    if (sell_rejection && !ctx.futures_enabled && !ctx.position.has_position) return std::nullopt;

    Side side = buy_rejection ? Side::Buy : Side::Sell;
    double atr = tech.atr_14;
    double stop = buy_rejection ? mid - atr * 2.5 : mid + atr * 2.5;

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
    int64_t age_ms = (now_ns - setup.detected_at_ns) / 1'000'000;
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

    // 3. Ликвидность ухудшилась
    if (micro.liquidity_valid && micro.liquidity_ratio < cfg_.min_liquidity_ratio * 0.5) {
        result.valid = false;
        result.conditions_degraded = true;
        result.reasons.push_back("liquidity_degraded");
        return result;
    }

    // 4. VPIN стал токсичным
    if (micro.vpin_valid && micro.vpin_toxic) {
        result.valid = false;
        result.trap_detected = true;
        result.reasons.push_back("vpin_became_toxic");
        return result;
    }

    // 5. Структура сломалась — imbalance развернулся
    bool imbalance_reversed = false;
    if (setup.side == Side::Buy && micro.book_imbalance_5 < -cfg_.imbalance_threshold) {
        imbalance_reversed = true;
    }
    if (setup.side == Side::Sell && micro.book_imbalance_5 > cfg_.imbalance_threshold) {
        imbalance_reversed = true;
    }
    if (imbalance_reversed) {
        result.valid = false;
        result.reasons.push_back("imbalance_reversed");
        return result;
    }

    // 6. Цена сильно ушла от reference (слишком далеко для входа)
    if (tech.atr_valid && tech.atr_14 > 0.0) {
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

    // Подтверждение: прошло достаточно времени + структура держится
    int64_t age_ms = (now_ns - setup.detected_at_ns) / 1'000'000;
    if (age_ms < cfg_.setup_confirmation_window_ms) {
        return false;
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
