#include "regime/regime_engine.hpp"
#include <cmath>

namespace tb::regime {

// ============================================================
// Преобразования
// ============================================================

std::string to_string(DetailedRegime regime) {
    switch (regime) {
        case DetailedRegime::StrongUptrend:       return "StrongUptrend";
        case DetailedRegime::WeakUptrend:         return "WeakUptrend";
        case DetailedRegime::StrongDowntrend:     return "StrongDowntrend";
        case DetailedRegime::WeakDowntrend:       return "WeakDowntrend";
        case DetailedRegime::MeanReversion:       return "MeanReversion";
        case DetailedRegime::VolatilityExpansion: return "VolatilityExpansion";
        case DetailedRegime::LowVolCompression:   return "LowVolCompression";
        case DetailedRegime::LiquidityStress:     return "LiquidityStress";
        case DetailedRegime::SpreadInstability:   return "SpreadInstability";
        case DetailedRegime::AnomalyEvent:        return "AnomalyEvent";
        case DetailedRegime::ToxicFlow:           return "ToxicFlow";
        case DetailedRegime::Chop:                return "Chop";
        case DetailedRegime::Undefined:           return "Undefined";
    }
    return "Undefined";
}

RegimeLabel to_simple_label(DetailedRegime regime) {
    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::WeakUptrend:
        case DetailedRegime::StrongDowntrend:
        case DetailedRegime::WeakDowntrend:
            return RegimeLabel::Trending;
        case DetailedRegime::MeanReversion:
        case DetailedRegime::LowVolCompression:
        case DetailedRegime::Chop:
            return RegimeLabel::Ranging;
        case DetailedRegime::VolatilityExpansion:
        case DetailedRegime::AnomalyEvent:
        case DetailedRegime::LiquidityStress:
        case DetailedRegime::SpreadInstability:
        case DetailedRegime::ToxicFlow:
            return RegimeLabel::Volatile;
        case DetailedRegime::Undefined:
            return RegimeLabel::Unclear;
    }
    return RegimeLabel::Unclear;
}

// ============================================================
// RuleBasedRegimeEngine
// ============================================================

RuleBasedRegimeEngine::RuleBasedRegimeEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{}

RegimeSnapshot RuleBasedRegimeEngine::classify(const features::FeatureSnapshot& snapshot) {
    const auto& sym = snapshot.symbol.get();

    auto detailed = classify_detailed(snapshot);
    auto confidence = compute_confidence(snapshot, detailed);

    // Определяем предыдущий режим для стабильности и перехода
    DetailedRegime previous = DetailedRegime::Undefined;
    {
        std::lock_guard lock(mutex_);
        auto it = snapshots_.find(sym);
        if (it != snapshots_.end()) {
            previous = it->second.detailed;
        }
    }

    auto stability = compute_stability(detailed, previous);
    auto hints = generate_hints(detailed);

    RegimeSnapshot result;
    result.label = to_simple_label(detailed);
    result.detailed = detailed;
    result.confidence = confidence;
    result.stability = stability;
    result.strategy_hints = std::move(hints);
    result.computed_at = clock_->now();
    result.symbol = snapshot.symbol;

    // Фиксируем переход режима
    if (previous != detailed && previous != DetailedRegime::Undefined) {
        RegimeTransition transition;
        transition.from = previous;
        transition.to = detailed;
        transition.confidence = confidence;
        transition.occurred_at = clock_->now();
        result.last_transition = transition;
    }

    // Сохраняем снимок
    {
        std::lock_guard lock(mutex_);
        snapshots_[sym] = result;
    }

    logger_->debug("Regime",
                   "Режим: " + to_string(detailed) + " для " + sym,
                   {{"regime", to_string(detailed)},
                    {"confidence", std::to_string(confidence)},
                    {"stability", std::to_string(stability)}});

    return result;
}

std::optional<RegimeSnapshot> RuleBasedRegimeEngine::current_regime(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = snapshots_.find(symbol.get());
    if (it != snapshots_.end()) {
        return it->second;
    }
    return std::nullopt;
}

DetailedRegime RuleBasedRegimeEngine::classify_detailed(const features::FeatureSnapshot& snap) const {
    const auto& tech = snap.technical;
    const auto& micro = snap.microstructure;

    // Проверяем наличие минимальных данных
    if (!tech.ema_valid && !tech.rsi_valid && !tech.adx_valid) {
        return DetailedRegime::Undefined;
    }

    // CUSUM: раннее обнаружение смены режима
    if (tech.cusum_valid && tech.cusum_regime_change) {
        // CUSUM обнаружил сдвиг — логируем, но не возвращаем конкретный режим,
        // чтобы дальнейшая классификация определила новый режим
        if (logger_) {
            logger_->info("Regime", "CUSUM обнаружил смену режима",
                {{"cusum_pos", std::to_string(tech.cusum_positive)},
                 {"cusum_neg", std::to_string(tech.cusum_negative)}});
        }
    }

    // --- AnomalyEvent: экстремальный RSI + экстремальный объём ---
    if (tech.rsi_valid && (tech.rsi_14 > 85.0 || tech.rsi_14 < 15.0) &&
        tech.obv_valid && std::abs(tech.obv_normalized) > 2.0) {
        return DetailedRegime::AnomalyEvent;
    }

    // --- VPIN ToxicFlow: более точная детекция через VPIN ---
    if (micro.vpin_valid && micro.vpin_toxic) {
        return DetailedRegime::ToxicFlow;
    }

    // --- ToxicFlow: aggressive_flow > 0.75, спред расширяется ---
    if (micro.trade_flow_valid && micro.aggressive_flow > 0.75 &&
        micro.spread_valid && micro.spread_bps > 15.0) {
        return DetailedRegime::ToxicFlow;
    }

    // --- SpreadInstability: book_instability > 0.6 ---
    if (micro.instability_valid && micro.book_instability > 0.6) {
        return DetailedRegime::SpreadInstability;
    }

    // --- LiquidityStress: широкий спред + низкая глубина ---
    if (micro.spread_valid && micro.spread_bps > 30.0 &&
        micro.liquidity_valid && micro.liquidity_ratio > 3.0) {
        return DetailedRegime::LiquidityStress;
    }

    // --- VolatilityExpansion: широкий BB bandwidth + ATR растёт ---
    if (tech.bb_valid && tech.bb_bandwidth > 0.06 &&
        tech.atr_valid && tech.atr_14_normalized > 0.02) {
        return DetailedRegime::VolatilityExpansion;
    }

    // --- LowVolCompression: узкий BB bandwidth + ADX < 20 ---
    if (tech.bb_valid && tech.bb_bandwidth < 0.02 &&
        tech.adx_valid && tech.adx < 20.0) {
        return DetailedRegime::LowVolCompression;
    }

    // --- MeanReversion: экстремальный RSI + ADX < 25 ---
    if (tech.rsi_valid && (tech.rsi_14 > 70.0 || tech.rsi_14 < 30.0) &&
        tech.adx_valid && tech.adx < 25.0) {
        return DetailedRegime::MeanReversion;
    }

    // --- Трендовые режимы на основе EMA и ADX ---
    if (tech.ema_valid && tech.adx_valid) {
        bool uptrend = tech.ema_20 > tech.ema_50;
        bool downtrend = tech.ema_20 < tech.ema_50;

        // StrongUptrend: EMA20 > EMA50, ADX > 30, RSI 50-70
        if (uptrend && tech.adx > 30.0 &&
            tech.rsi_valid && tech.rsi_14 >= 50.0 && tech.rsi_14 <= 70.0) {
            return DetailedRegime::StrongUptrend;
        }

        // WeakUptrend: EMA20 > EMA50, ADX 20-30
        if (uptrend && tech.adx >= 20.0 && tech.adx <= 30.0) {
            return DetailedRegime::WeakUptrend;
        }

        // StrongDowntrend: EMA20 < EMA50, ADX > 30, RSI 30-50
        if (downtrend && tech.adx > 30.0 &&
            tech.rsi_valid && tech.rsi_14 >= 30.0 && tech.rsi_14 <= 50.0) {
            return DetailedRegime::StrongDowntrend;
        }

        // WeakDowntrend: EMA20 < EMA50, ADX 20-30
        if (downtrend && tech.adx >= 20.0 && tech.adx <= 30.0) {
            return DetailedRegime::WeakDowntrend;
        }
    }

    // --- Chop: ADX < 18, нет выраженного направления ---
    if (tech.adx_valid && tech.adx < 18.0) {
        return DetailedRegime::Chop;
    }

    return DetailedRegime::Undefined;
}

double RuleBasedRegimeEngine::compute_confidence(
    const features::FeatureSnapshot& snap, DetailedRegime regime) const {

    const auto& tech = snap.technical;
    double confidence = 0.5; // Базовая уверенность

    // Больше валидных индикаторов → выше уверенность
    int valid_count = 0;
    if (tech.ema_valid) ++valid_count;
    if (tech.rsi_valid) ++valid_count;
    if (tech.adx_valid) ++valid_count;
    if (tech.bb_valid) ++valid_count;
    if (tech.macd_valid) ++valid_count;
    if (snap.microstructure.spread_valid) ++valid_count;

    // Корректировка за количество данных
    confidence += (valid_count / 6.0) * 0.2;

    // Уверенность зависит от режима
    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::StrongDowntrend:
            // Сильный тренд — высокая уверенность при высоком ADX
            if (tech.adx_valid) {
                confidence += std::min(0.2, (tech.adx - 30.0) / 50.0 * 0.2);
            }
            break;
        case DetailedRegime::AnomalyEvent:
            confidence = 0.9; // Аномалия очевидна
            break;
        case DetailedRegime::Chop:
        case DetailedRegime::Undefined:
            confidence = std::max(0.2, confidence - 0.1);
            break;
        default:
            break;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

double RuleBasedRegimeEngine::compute_stability(
    DetailedRegime current, DetailedRegime previous) const {

    if (previous == DetailedRegime::Undefined) {
        return 0.5; // Первая классификация — средняя стабильность
    }

    if (current == previous) {
        return 0.9; // Режим не изменился — высокая стабильность
    }

    // Переход внутри трендовой группы — умеренная стабильность
    auto is_trend = [](DetailedRegime r) {
        return r == DetailedRegime::StrongUptrend || r == DetailedRegime::WeakUptrend ||
               r == DetailedRegime::StrongDowntrend || r == DetailedRegime::WeakDowntrend;
    };

    if (is_trend(current) && is_trend(previous)) {
        return 0.6;
    }

    return 0.3; // Переход между группами — низкая стабильность
}

std::vector<RegimeStrategyHint> RuleBasedRegimeEngine::generate_hints(DetailedRegime regime) const {
    std::vector<RegimeStrategyHint> hints;

    auto add = [&](const std::string& id, bool enable, double weight, const std::string& reason) {
        hints.push_back({StrategyId(id), enable, weight, reason});
    };

    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::StrongDowntrend:
            add("momentum",            true,  1.5, "Сильный тренд — momentum эффективен");
            add("mean_reversion",      false, 0.0, "Сильный тренд — mean reversion отключён");
            add("breakout",            true,  0.8, "Продолжение тренда возможно");
            add("microstructure_scalp",true,  0.6, "Скальпинг в направлении тренда");
            add("vol_expansion",       true,  0.7, "Волатильность может расти");
            break;
        case DetailedRegime::WeakUptrend:
        case DetailedRegime::WeakDowntrend:
            add("momentum",            true,  0.8, "Слабый тренд — momentum с пониженным весом");
            add("mean_reversion",      true,  0.5, "Слабый тренд — mean reversion возможен");
            add("breakout",            true,  0.6, "Пробой из слабого тренда");
            add("microstructure_scalp",true,  0.8, "Скальпинг допустим");
            add("vol_expansion",       true,  0.5, "Умеренная вероятность расширения");
            break;
        case DetailedRegime::MeanReversion:
            add("momentum",            false, 0.0, "Разворот — momentum опасен");
            add("mean_reversion",      true,  1.5, "Идеальный сценарий для mean reversion");
            add("breakout",            false, 0.0, "Возврат — не пробой");
            add("microstructure_scalp",true,  0.7, "Скальпинг на возврате");
            add("vol_expansion",       false, 0.0, "Сжатие волатильности ожидается");
            break;
        case DetailedRegime::VolatilityExpansion:
            add("momentum",            true,  0.7, "Направленная волатильность");
            add("mean_reversion",      false, 0.0, "Волатильность — mean reversion опасен");
            add("breakout",            true,  1.2, "Расширение — хорошо для пробоев");
            add("microstructure_scalp",false, 0.0, "Волатильный стакан — скальпинг отключён");
            add("vol_expansion",       true,  1.5, "Идеальный сценарий");
            break;
        case DetailedRegime::LowVolCompression:
            add("momentum",            false, 0.0, "Нет импульса в сжатии");
            add("mean_reversion",      true,  1.0, "Диапазонная торговля в сжатии");
            add("breakout",            true,  1.5, "Сжатие → ожидание пробоя");
            add("microstructure_scalp",true,  1.0, "Узкие спреды — хорошо для скальпинга");
            add("vol_expansion",       true,  1.2, "Подготовка к расширению");
            break;
        case DetailedRegime::LiquidityStress:
        case DetailedRegime::SpreadInstability:
        case DetailedRegime::ToxicFlow:
            add("momentum",            false, 0.0, "Стресс — все стратегии отключены");
            add("mean_reversion",      false, 0.0, "Стресс — все стратегии отключены");
            add("breakout",            false, 0.0, "Стресс — все стратегии отключены");
            add("microstructure_scalp",false, 0.0, "Стресс — все стратегии отключены");
            add("vol_expansion",       false, 0.0, "Стресс — все стратегии отключены");
            break;
        case DetailedRegime::AnomalyEvent:
            add("momentum",            false, 0.0, "Аномалия — торговля приостановлена");
            add("mean_reversion",      true,  0.5, "Возврат после аномалии");
            add("breakout",            false, 0.0, "Ложные пробои при аномалии");
            add("microstructure_scalp",false, 0.0, "Микроструктура нестабильна");
            add("vol_expansion",       false, 0.0, "Волатильность уже экстремальна");
            break;
        case DetailedRegime::Chop:
            add("momentum",            false, 0.0, "Рубка — momentum неэффективен");
            add("mean_reversion",      true,  1.2, "Рубка — mean reversion хорош");
            add("breakout",            false, 0.0, "Ложные пробои в рубке");
            add("microstructure_scalp",true,  1.3, "Скальпинг на шуме");
            add("vol_expansion",       false, 0.0, "Нет расширения волатильности");
            break;
        case DetailedRegime::Undefined:
            add("momentum",            true,  0.5, "Режим не определён — пониженный вес");
            add("mean_reversion",      true,  0.5, "Режим не определён — пониженный вес");
            add("breakout",            true,  0.5, "Режим не определён — пониженный вес");
            add("microstructure_scalp",true,  0.5, "Режим не определён — пониженный вес");
            add("vol_expansion",       true,  0.5, "Режим не определён — пониженный вес");
            break;
    }

    return hints;
}

} // namespace tb::regime
