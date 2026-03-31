#include "regime/regime_engine.hpp"
#include <cmath>
#include <sstream>

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
// RuleBasedRegimeEngine — конструктор
// ============================================================

RuleBasedRegimeEngine::RuleBasedRegimeEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    RegimeConfig config)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{}

// ============================================================
// Основной метод классификации
// ============================================================

RegimeSnapshot RuleBasedRegimeEngine::classify(const features::FeatureSnapshot& snapshot) {
    const auto& sym = snapshot.symbol.get();

    ClassificationExplanation explanation;

    // Мгновенная rule-based классификация
    auto immediate = classify_immediate(snapshot, explanation);
    explanation.immediate_regime = immediate;

    // Предыдущий режим и hysteresis (thread-safe)
    DetailedRegime previous = DetailedRegime::Undefined;
    DetailedRegime final_regime;
    {
        std::lock_guard lock(mutex_);

        auto snap_it = snapshots_.find(sym);
        if (snap_it != snapshots_.end()) {
            previous = snap_it->second.detailed;
        }

        // Предварительная уверенность для решения о переключении
        auto confidence_for_switch = compute_confidence(snapshot, immediate, explanation);

        auto& hyst = hysteresis_[sym];
        final_regime = apply_hysteresis(immediate, confidence_for_switch, hyst, explanation);
    }
    explanation.persistent_regime = final_regime;

    // Итоговые метрики по финальному режиму
    auto confidence = compute_confidence(snapshot, final_regime, explanation);
    auto stability  = compute_stability(final_regime, previous);
    auto hints      = generate_hints(final_regime);
    auto summary    = build_summary(final_regime, confidence, stability,
                                    explanation.hysteresis_overrode);
    explanation.summary = summary;

    // Собираем снимок результата
    RegimeSnapshot result;
    result.label           = to_simple_label(final_regime);
    result.detailed        = final_regime;
    result.confidence      = confidence;
    result.stability       = stability;
    result.strategy_hints  = std::move(hints);
    result.computed_at     = clock_->now();
    result.symbol          = snapshot.symbol;
    result.explanation     = std::move(explanation);

    // Фиксируем переход режима
    if (previous != final_regime && previous != DetailedRegime::Undefined) {
        RegimeTransition transition;
        transition.from        = previous;
        transition.to          = final_regime;
        transition.confidence  = confidence;
        transition.occurred_at = clock_->now();
        result.last_transition = transition;
    }

    // Сохраняем снимок
    {
        std::lock_guard lock(mutex_);
        snapshots_[sym] = result;
    }

    logger_->debug("Regime",
                   "Режим: " + to_string(final_regime) + " для " + sym,
                   {{"regime",     to_string(final_regime)},
                    {"confidence", std::to_string(confidence)},
                    {"stability",  std::to_string(stability)}});

    return result;
}

// ============================================================
// Текущий режим
// ============================================================

std::optional<RegimeSnapshot> RuleBasedRegimeEngine::current_regime(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = snapshots_.find(symbol.get());
    if (it != snapshots_.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================
// Мгновенная rule-based классификация (без hysteresis)
// ============================================================

DetailedRegime RuleBasedRegimeEngine::classify_immediate(
    const features::FeatureSnapshot& snap,
    ClassificationExplanation& explanation) const {

    const auto& tech  = snap.technical;
    const auto& micro = snap.microstructure;

    // Хелпер: регистрирует проверку условия и возвращает результат
    auto check = [&](const std::string& indicator, double value, double threshold,
                     const std::string& op, bool triggered) -> bool {
        explanation.checked_conditions.push_back({indicator, value, threshold, op, triggered});
        if (triggered) {
            explanation.triggered_conditions.push_back({indicator, value, threshold, op, true});
        }
        return triggered;
    };

    // Подсчёт валидных индикаторов (ядро, используемое в confidence)
    int valid = 0;
    if (tech.ema_valid)        ++valid;
    if (tech.rsi_valid)        ++valid;
    if (tech.adx_valid)        ++valid;
    if (tech.bb_valid)         ++valid;
    if (tech.macd_valid)       ++valid;
    if (micro.spread_valid)    ++valid;

    explanation.valid_indicator_count = valid;
    explanation.total_indicator_count = config_.confidence.max_indicator_count;
    explanation.data_quality_score    = static_cast<double>(valid) /
        std::max(1, config_.confidence.max_indicator_count);

    // --- Проверяем наличие минимальных данных ---
    if (!tech.ema_valid && !tech.rsi_valid && !tech.adx_valid) {
        return DetailedRegime::Undefined;
    }

    // --- CUSUM: раннее обнаружение смены режима ---
    if (tech.cusum_valid && tech.cusum_regime_change) {
        check("cusum_regime_change", 1.0, 1.0, "==", true);
        if (logger_) {
            logger_->info("Regime", "CUSUM обнаружил смену режима",
                {{"cusum_pos", std::to_string(tech.cusum_positive)},
                 {"cusum_neg", std::to_string(tech.cusum_negative)}});
        }
    }

    // --- AnomalyEvent: экстремальный RSI + экстремальный объём ---
    if (tech.rsi_valid && tech.obv_valid) {
        bool rsi_high = check("RSI", tech.rsi_14, config_.stress.rsi_extreme_high, ">",
                              tech.rsi_14 > config_.stress.rsi_extreme_high);
        bool rsi_low  = check("RSI", tech.rsi_14, config_.stress.rsi_extreme_low, "<",
                              tech.rsi_14 < config_.stress.rsi_extreme_low);
        bool obv_ext  = check("OBV_norm", std::abs(tech.obv_normalized),
                              config_.stress.obv_norm_extreme, ">",
                              std::abs(tech.obv_normalized) > config_.stress.obv_norm_extreme);

        if ((rsi_high || rsi_low) && obv_ext) {
            return DetailedRegime::AnomalyEvent;
        }
    }

    // --- VPIN ToxicFlow ---
    if (micro.vpin_valid) {
        if (check("VPIN_toxic", micro.vpin_toxic ? 1.0 : 0.0, 1.0, "==", micro.vpin_toxic)) {
            return DetailedRegime::ToxicFlow;
        }
    }

    // --- ToxicFlow: aggressive_flow + спред ---
    if (micro.trade_flow_valid && micro.spread_valid) {
        bool flow_toxic   = check("aggressive_flow", micro.aggressive_flow,
                                  config_.stress.aggressive_flow_toxic, ">",
                                  micro.aggressive_flow > config_.stress.aggressive_flow_toxic);
        bool spread_toxic = check("spread_bps", micro.spread_bps,
                                  config_.stress.spread_toxic_bps, ">",
                                  micro.spread_bps > config_.stress.spread_toxic_bps);
        if (flow_toxic && spread_toxic) {
            return DetailedRegime::ToxicFlow;
        }
    }

    // --- SpreadInstability ---
    if (micro.instability_valid) {
        if (check("book_instability", micro.book_instability,
                  config_.stress.book_instability_threshold, ">",
                  micro.book_instability > config_.stress.book_instability_threshold)) {
            return DetailedRegime::SpreadInstability;
        }
    }

    // --- LiquidityStress: широкий спред + низкая глубина ---
    if (micro.spread_valid && micro.liquidity_valid) {
        bool spread_stress = check("spread_bps", micro.spread_bps,
                                   config_.stress.spread_stress_bps, ">",
                                   micro.spread_bps > config_.stress.spread_stress_bps);
        bool liq_stress    = check("liquidity_ratio", micro.liquidity_ratio,
                                   config_.stress.liquidity_ratio_stress, ">",
                                   micro.liquidity_ratio > config_.stress.liquidity_ratio_stress);
        if (spread_stress && liq_stress) {
            return DetailedRegime::LiquidityStress;
        }
    }

    // --- VolatilityExpansion: широкий BB bandwidth + ATR растёт ---
    if (tech.bb_valid && tech.atr_valid) {
        bool bb_exp  = check("bb_bandwidth", tech.bb_bandwidth,
                             config_.volatility.bb_bandwidth_expansion, ">",
                             tech.bb_bandwidth > config_.volatility.bb_bandwidth_expansion);
        bool atr_exp = check("atr_normalized", tech.atr_14_normalized,
                             config_.volatility.atr_norm_expansion, ">",
                             tech.atr_14_normalized > config_.volatility.atr_norm_expansion);
        if (bb_exp && atr_exp) {
            return DetailedRegime::VolatilityExpansion;
        }
    }

    // --- LowVolCompression: узкий BB bandwidth + ADX < порога ---
    if (tech.bb_valid && tech.adx_valid) {
        bool bb_comp  = check("bb_bandwidth", tech.bb_bandwidth,
                              config_.volatility.bb_bandwidth_compression, "<",
                              tech.bb_bandwidth < config_.volatility.bb_bandwidth_compression);
        bool adx_comp = check("ADX", tech.adx,
                              config_.volatility.adx_compression_max, "<",
                              tech.adx < config_.volatility.adx_compression_max);
        if (bb_comp && adx_comp) {
            return DetailedRegime::LowVolCompression;
        }
    }

    // --- MeanReversion: экстремальный RSI + низкий ADX ---
    if (tech.rsi_valid && tech.adx_valid) {
        bool rsi_ob  = check("RSI", tech.rsi_14,
                             config_.mean_reversion.rsi_overbought, ">",
                             tech.rsi_14 > config_.mean_reversion.rsi_overbought);
        bool rsi_os  = check("RSI", tech.rsi_14,
                             config_.mean_reversion.rsi_oversold, "<",
                             tech.rsi_14 < config_.mean_reversion.rsi_oversold);
        bool adx_low = check("ADX", tech.adx,
                             config_.mean_reversion.adx_max, "<",
                             tech.adx < config_.mean_reversion.adx_max);
        if ((rsi_ob || rsi_os) && adx_low) {
            return DetailedRegime::MeanReversion;
        }
    }

    // --- Трендовые режимы на основе EMA и ADX ---
    if (tech.ema_valid && tech.adx_valid) {
        bool uptrend   = tech.ema_20 > tech.ema_50;
        bool downtrend = tech.ema_20 < tech.ema_50;

        if (uptrend) {
            check("EMA_cross", tech.ema_20 - tech.ema_50, 0.0, ">", true);

            // StrongUptrend: EMA20 > EMA50, ADX > adx_strong, RSI >= rsi_trend_bias
            bool adx_strong = check("ADX", tech.adx, config_.trend.adx_strong, ">",
                                    tech.adx > config_.trend.adx_strong);
            if (adx_strong && tech.rsi_valid) {
                bool rsi_bullish = check("RSI", tech.rsi_14, config_.trend.rsi_trend_bias, ">=",
                                         tech.rsi_14 >= config_.trend.rsi_trend_bias);
                if (rsi_bullish) {
                    return DetailedRegime::StrongUptrend;
                }
            }

            // WeakUptrend: EMA20 > EMA50, ADX in [adx_weak_min, adx_weak_max]
            if (check("ADX", tech.adx, config_.trend.adx_weak_min, "in_range",
                      tech.adx >= config_.trend.adx_weak_min &&
                      tech.adx <= config_.trend.adx_weak_max)) {
                return DetailedRegime::WeakUptrend;
            }
        }

        if (downtrend) {
            check("EMA_cross", tech.ema_20 - tech.ema_50, 0.0, "<", true);

            // StrongDowntrend: EMA20 < EMA50, ADX > adx_strong, RSI <= rsi_trend_bias
            bool adx_strong = check("ADX", tech.adx, config_.trend.adx_strong, ">",
                                    tech.adx > config_.trend.adx_strong);
            if (adx_strong && tech.rsi_valid) {
                bool rsi_bearish = check("RSI", tech.rsi_14, config_.trend.rsi_trend_bias, "<=",
                                         tech.rsi_14 <= config_.trend.rsi_trend_bias);
                if (rsi_bearish) {
                    return DetailedRegime::StrongDowntrend;
                }
            }

            // WeakDowntrend: EMA20 < EMA50, ADX in [adx_weak_min, adx_weak_max]
            if (check("ADX", tech.adx, config_.trend.adx_weak_min, "in_range",
                      tech.adx >= config_.trend.adx_weak_min &&
                      tech.adx <= config_.trend.adx_weak_max)) {
                return DetailedRegime::WeakDowntrend;
            }
        }
    }

    // --- Chop: ADX < порога, нет выраженного направления ---
    if (tech.adx_valid) {
        if (check("ADX", tech.adx, config_.chop.adx_max, "<",
                  tech.adx < config_.chop.adx_max)) {
            return DetailedRegime::Chop;
        }
    }

    return DetailedRegime::Undefined;
}

// ============================================================
// Hysteresis: сглаживание переходов между режимами
// ============================================================

DetailedRegime RuleBasedRegimeEngine::apply_hysteresis(
    DetailedRegime immediate, double confidence,
    HysteresisState& state,
    ClassificationExplanation& explanation) const {

    DetailedRegime result;

    if (state.confirmed_regime == DetailedRegime::Undefined) {
        // Первая классификация — принимаем сразу
        state.confirmed_regime = immediate;
        state.candidate_regime = DetailedRegime::Undefined;
        state.candidate_ticks  = 0;
        state.dwell_ticks      = 1;
        result = immediate;

    } else if (immediate == state.confirmed_regime) {
        // Мгновенный == подтверждённый — стабильное состояние
        state.candidate_regime = DetailedRegime::Undefined;
        state.candidate_ticks  = 0;
        ++state.dwell_ticks;
        result = state.confirmed_regime;

    } else if (immediate != state.candidate_regime) {
        // Новый кандидат, отличный от текущего candidate
        state.candidate_regime = immediate;
        state.candidate_ticks  = 1;
        result = state.confirmed_regime;

    } else {
        // Кандидат повторяется — наращиваем счётчик
        ++state.candidate_ticks;

        bool enough_ticks = state.candidate_ticks >= config_.transition.confirmation_ticks;
        bool enough_conf  = confidence >= config_.transition.min_confidence_to_switch;
        bool enough_dwell = state.dwell_ticks >= config_.transition.dwell_time_ticks;

        if (enough_ticks && enough_conf && enough_dwell) {
            // Подтверждаем переход
            state.confirmed_regime = state.candidate_regime;
            state.candidate_regime = DetailedRegime::Undefined;
            state.candidate_ticks  = 0;
            state.dwell_ticks      = 1;
            result = state.confirmed_regime;
        } else {
            result = state.confirmed_regime;
        }
    }

    explanation.hysteresis_overrode = (immediate != result);
    explanation.confirmation_ticks_remaining =
        std::max(0, config_.transition.confirmation_ticks - state.candidate_ticks);
    explanation.dwell_ticks = state.dwell_ticks;

    return result;
}

// ============================================================
// Уверенность классификации
// ============================================================

double RuleBasedRegimeEngine::compute_confidence(
    const features::FeatureSnapshot& snap,
    DetailedRegime regime,
    const ClassificationExplanation& explanation) const {

    const auto& tech = snap.technical;
    const auto& conf = config_.confidence;

    double confidence = conf.base_confidence;

    // Корректировка за качество данных
    confidence += explanation.data_quality_score * conf.data_quality_weight;

    // Режимо-зависимая корректировка
    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::StrongDowntrend:
            if (tech.adx_valid) {
                confidence += std::min(conf.strong_trend_bonus_scale,
                    (tech.adx - config_.trend.adx_strong) / 50.0 * conf.strong_trend_bonus_scale);
            }
            break;
        case DetailedRegime::AnomalyEvent:
            confidence = conf.anomaly_confidence;
            break;
        case DetailedRegime::Chop:
        case DetailedRegime::Undefined:
            confidence = std::max(0.2, confidence - conf.weak_regime_penalty);
            break;
        default:
            break;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

// ============================================================
// Стабильность режима
// ============================================================

double RuleBasedRegimeEngine::compute_stability(
    DetailedRegime current, DetailedRegime previous) const {

    const auto& conf = config_.confidence;

    if (previous == DetailedRegime::Undefined) {
        return conf.first_classification_stability;
    }

    if (current == previous) {
        return conf.same_regime_stability;
    }

    // Переход внутри трендовой группы — умеренная стабильность
    auto is_trend = [](DetailedRegime r) {
        return r == DetailedRegime::StrongUptrend  || r == DetailedRegime::WeakUptrend ||
               r == DetailedRegime::StrongDowntrend || r == DetailedRegime::WeakDowntrend;
    };

    if (is_trend(current) && is_trend(previous)) {
        return conf.intra_group_stability;
    }

    return conf.inter_group_stability;
}

// ============================================================
// Стратегические рекомендации
// ============================================================

std::vector<RegimeStrategyHint> RuleBasedRegimeEngine::generate_hints(DetailedRegime regime) const {
    std::vector<RegimeStrategyHint> hints;

    auto add = [&](const std::string& id, bool enable, double weight, const std::string& reason) {
        hints.push_back({StrategyId(id), enable, weight, reason});
    };

    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::StrongDowntrend:
            add("momentum",            true,  1.5, "Сильный тренд — momentum эффективен");
            add("mean_reversion",      true,  0.15,"Сильный тренд — mean reversion пониженный");
            add("breakout",            true,  0.8, "Продолжение тренда возможно");
            add("microstructure_scalp",true,  0.6, "Скальпинг в направлении тренда");
            add("vol_expansion",       true,  0.7, "Волатильность может расти");
            add("ema_pullback",        true,  1.3, "Pullback в сильном тренде — отлично");
            add("rsi_divergence",      true,  0.4, "Дивергенции против тренда рискованны");
            add("vwap_reversion",      true,  0.5, "VWAP reversion — осторожно в тренде");
            add("volume_profile",      true,  0.6, "Объёмные уровни как поддержка");
            break;
        case DetailedRegime::WeakUptrend:
        case DetailedRegime::WeakDowntrend:
            add("momentum",            true,  0.8, "Слабый тренд — momentum с пониженным весом");
            add("mean_reversion",      true,  0.5, "Слабый тренд — mean reversion возможен");
            add("breakout",            true,  0.6, "Пробой из слабого тренда");
            add("microstructure_scalp",true,  0.8, "Скальпинг допустим");
            add("vol_expansion",       true,  0.5, "Умеренная вероятность расширения");
            add("ema_pullback",        true,  1.0, "Pullback в слабом тренде — хорошо");
            add("rsi_divergence",      true,  0.7, "Дивергенции возможны");
            add("vwap_reversion",      true,  0.7, "VWAP reversion допустим");
            add("volume_profile",      true,  0.7, "Объёмные уровни актуальны");
            break;
        case DetailedRegime::MeanReversion:
            add("momentum",            true,  0.2, "Разворот — momentum пониженный");
            add("mean_reversion",      true,  1.5, "Идеальный сценарий для mean reversion");
            add("breakout",            true,  0.15,"Возврат — breakout маловероятен");
            add("microstructure_scalp",true,  0.7, "Скальпинг на возврате");
            add("vol_expansion",       true,  0.15,"Сжатие волатильности ожидается");
            add("ema_pullback",        true,  0.5, "Pullback в reversion — средне");
            add("rsi_divergence",      true,  1.2, "RSI дивергенция при развороте — отлично");
            add("vwap_reversion",      true,  1.3, "VWAP reversion — отлично при развороте");
            add("volume_profile",      true,  1.0, "Объёмные уровни при развороте");
            break;
        case DetailedRegime::VolatilityExpansion:
            add("momentum",            true,  0.7, "Направленная волатильность");
            add("mean_reversion",      true,  0.15,"Волатильность — mean reversion рискован");
            add("breakout",            true,  1.2, "Расширение — хорошо для пробоев");
            add("microstructure_scalp",true,  0.2, "Волатильный стакан — скальпинг осторожно");
            add("vol_expansion",       true,  1.5, "Идеальный сценарий");
            add("ema_pullback",        true,  0.5, "Pullback при vol expansion — рискованно");
            add("rsi_divergence",      true,  0.5, "Дивергенции при волатильности");
            add("vwap_reversion",      true,  0.2, "VWAP менее актуален при vol expansion");
            add("volume_profile",      true,  0.5, "Объёмные уровни как ориентир");
            break;
        case DetailedRegime::LowVolCompression:
            add("momentum",            true,  0.2, "Слабый импульс в сжатии");
            add("mean_reversion",      true,  1.0, "Диапазонная торговля в сжатии");
            add("breakout",            true,  1.5, "Сжатие → ожидание пробоя");
            add("microstructure_scalp",true,  1.0, "Узкие спреды — хорошо для скальпинга");
            add("vol_expansion",       true,  1.2, "Подготовка к расширению");
            add("ema_pullback",        true,  0.5, "Pullback в сжатии — средне");
            add("rsi_divergence",      true,  0.8, "Дивергенции в сжатии — хорошо");
            add("vwap_reversion",      true,  1.0, "VWAP reversion в диапазоне");
            add("volume_profile",      true,  1.2, "POC/VA актуальны в сжатии");
            break;
        case DetailedRegime::LiquidityStress:
        case DetailedRegime::SpreadInstability:
        case DetailedRegime::ToxicFlow:
            // Стрессовые режимы: сильно пониженные веса, но ни одна не отключена полностью
            add("momentum",            true,  0.15,"Стресс — momentum минимальный");
            add("mean_reversion",      true,  0.3, "Стресс — mean reversion на отскоке");
            add("breakout",            true,  0.1, "Стресс — breakout минимальный");
            add("microstructure_scalp",true,  0.1, "Стресс — скальпинг минимальный");
            add("vol_expansion",       true,  0.1, "Стресс — vol_expansion минимальный");
            add("ema_pullback",        true,  0.1, "Стресс — pullback минимальный");
            add("rsi_divergence",      true,  0.3, "Стресс — дивергенции для отскока");
            add("vwap_reversion",      true,  0.25,"Стресс — VWAP reversion осторожно");
            add("volume_profile",      true,  0.1, "Стресс — profiles пониженный");
            break;
        case DetailedRegime::AnomalyEvent:
            add("momentum",            true,  0.1, "Аномалия — momentum минимальный");
            add("mean_reversion",      true,  0.5, "Возврат после аномалии");
            add("breakout",            true,  0.1, "Ложные пробои при аномалии");
            add("microstructure_scalp",true,  0.1, "Микроструктура нестабильна");
            add("vol_expansion",       true,  0.1, "Волатильность уже экстремальна");
            add("ema_pullback",        true,  0.1, "Аномалия — pullback минимальный");
            add("rsi_divergence",      true,  0.5, "Дивергенция после аномалии — средне");
            add("vwap_reversion",      true,  0.3, "VWAP reversion после аномалии");
            add("volume_profile",      true,  0.1, "Profiles ненадёжны при аномалии");
            break;
        case DetailedRegime::Chop:
            add("momentum",            true,  0.2, "Рубка — momentum пониженный");
            add("mean_reversion",      true,  1.2, "Рубка — mean reversion хорош");
            add("breakout",            true,  0.15,"Ложные пробои в рубке");
            add("microstructure_scalp",true,  1.3, "Скальпинг на шуме");
            add("vol_expansion",       true,  0.15,"Нет расширения волатильности");
            add("ema_pullback",        true,  0.2, "Слабые pullback без тренда");
            add("rsi_divergence",      true,  0.8, "RSI дивергенции в рубке — хорошо");
            add("vwap_reversion",      true,  1.2, "VWAP reversion в рубке — отлично");
            add("volume_profile",      true,  1.2, "POC/VA в боковике");
            break;
        case DetailedRegime::Undefined:
            add("momentum",            true,  0.5, "Режим не определён — пониженный вес");
            add("mean_reversion",      true,  0.5, "Режим не определён — пониженный вес");
            add("breakout",            true,  0.5, "Режим не определён — пониженный вес");
            add("microstructure_scalp",true,  0.5, "Режим не определён — пониженный вес");
            add("vol_expansion",       true,  0.5, "Режим не определён — пониженный вес");
            add("ema_pullback",        true,  0.5, "Режим не определён — пониженный вес");
            add("rsi_divergence",      true,  0.5, "Режим не определён — пониженный вес");
            add("vwap_reversion",      true,  0.5, "Режим не определён — пониженный вес");
            add("volume_profile",      true,  0.5, "Режим не определён — пониженный вес");
            break;
    }

    return hints;
}

// ============================================================
// Человекочитаемое резюме
// ============================================================

std::string RuleBasedRegimeEngine::build_summary(
    DetailedRegime regime, double confidence,
    double stability, bool hysteresis_overrode) {

    std::ostringstream oss;
    oss << std::fixed;
    oss.precision(2);
    oss << to_string(regime)
        << " (conf=" << confidence
        << ", stab=" << stability;
    if (hysteresis_overrode) {
        oss << ", hysteresis active";
    }
    oss << ")";
    return oss.str();
}

} // namespace tb::regime
