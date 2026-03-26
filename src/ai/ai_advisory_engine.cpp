/**
 * @file ai_advisory_engine.cpp
 * @brief Реализация локального AI Advisory на основе 14 правил
 *
 * Улучшения v2:
 * - Per-detector cooldown (индекс 0-13, а не per-type)
 * - Severity-weighted confidence aggregation
 * - Ensemble escalation: N детекторов → +bonus к max_severity
 * - Hysteresis state machine: Clear / Caution / Veto
 * - Жёсткий deadline: timeout_ms через chrono steady_clock
 * - advisory_size_multiplier: caution → 0.5, veto → 0.0, clear → 1.0
 */
#include "ai/ai_advisory_engine.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>

namespace tb::ai {

// ============================================================
// Construction & infrastructure
// ============================================================

LocalRuleBasedAdvisory::LocalRuleBasedAdvisory(
    AIAdvisoryConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
{
    status_.available = config_.enabled;
    status_.healthy = config_.enabled;
    cooldown_tracker_.fill(0);
    init_metrics();

    if (logger_ && config_.enabled) {
        logger_->info("AIAdvisory", "AI Advisory Engine инициализирован (v2)",
            {{"enabled", "true"},
             {"max_adj", std::to_string(config_.max_confidence_adjustment)},
             {"cooldown_ms", std::to_string(config_.cooldown_ms)},
             {"veto_threshold", std::to_string(config_.veto_severity_threshold)},
             {"caution_threshold", std::to_string(config_.caution_severity_threshold)},
             {"hysteresis_ticks", std::to_string(config_.hysteresis_clear_ticks)},
             {"ensemble_count", std::to_string(config_.ensemble_escalation_count)},
             {"severity_weighted", config_.use_severity_weighted_adjustment ? "true" : "false"}});
    }
}

void LocalRuleBasedAdvisory::init_metrics() {
    if (!metrics_) return;
    metric_requests_ = metrics_->counter("ai_advisory_requests_total");
    metric_hits_ = metrics_->counter("ai_advisory_hits_total");
    metric_anomaly_warnings_ = metrics_->counter("ai_advisory_anomaly_warnings_total");
    metric_risk_warnings_ = metrics_->counter("ai_advisory_risk_warnings_total");
    metric_strategy_hints_ = metrics_->counter("ai_advisory_strategy_hints_total");
    metric_processing_ms_ = metrics_->histogram(
        "ai_advisory_processing_ms", {0.1, 0.5, 1.0, 5.0, 10.0, 50.0});
    metric_confidence_adj_ = metrics_->gauge("ai_advisory_confidence_adjustment");
}

bool LocalRuleBasedAdvisory::is_on_cooldown(int detector_idx, int64_t now_ns) const {
    int64_t last = cooldown_tracker_[static_cast<size_t>(detector_idx)];
    if (last == 0) return false;
    int64_t cooldown_ns = config_.cooldown_ms * 1'000'000LL;
    return (now_ns - last) < cooldown_ns;
}

void LocalRuleBasedAdvisory::update_cooldown(int detector_idx, int64_t now_ns) {
    cooldown_tracker_[static_cast<size_t>(detector_idx)] = now_ns;
}

double LocalRuleBasedAdvisory::clamp_adjustment(double adj) const {
    return std::clamp(adj, -config_.max_confidence_adjustment, config_.max_confidence_adjustment);
}

// ============================================================
// Main entry points
// ============================================================

AIAdvisoryResult LocalRuleBasedAdvisory::get_advisories(
    const features::FeatureSnapshot& snapshot)
{
    AIAdvisoryResult result;
    if (!config_.enabled) return result;

    std::lock_guard<std::mutex> lock(mutex_);
    status_.requests_total++;
    if (metric_requests_) metric_requests_->increment();

    auto start = std::chrono::steady_clock::now();
    int64_t now_ns = snapshot.computed_at.get();

    // Жёсткий deadline — детекторы не должны блокировать горячий путь дольше timeout_ms
    auto deadline = start + std::chrono::milliseconds(config_.timeout_ms);

    using AnalyzeFn = std::optional<AIAdvisory> (LocalRuleBasedAdvisory::*)(
        const features::FeatureSnapshot&) const;

    static constexpr AnalyzeFn analyzers[] = {
        &LocalRuleBasedAdvisory::analyze_extreme_volatility,      // idx 0
        &LocalRuleBasedAdvisory::analyze_rsi_extreme,             // idx 1
        &LocalRuleBasedAdvisory::analyze_spread_anomaly,          // idx 2
        &LocalRuleBasedAdvisory::analyze_liquidity_concern,       // idx 3
        &LocalRuleBasedAdvisory::analyze_vpin_toxic_flow,         // idx 4
        &LocalRuleBasedAdvisory::analyze_volume_profile_anomaly,  // idx 5
        &LocalRuleBasedAdvisory::analyze_macd_divergence,         // idx 6
        &LocalRuleBasedAdvisory::analyze_bollinger_squeeze,       // idx 7
        &LocalRuleBasedAdvisory::analyze_adx_trend_strength,      // idx 8
        &LocalRuleBasedAdvisory::analyze_book_imbalance,          // idx 9
        &LocalRuleBasedAdvisory::analyze_time_of_day_risk,        // idx 10
        &LocalRuleBasedAdvisory::analyze_cusum_regime_change,     // idx 11
        &LocalRuleBasedAdvisory::analyze_momentum_divergence,     // idx 12
        &LocalRuleBasedAdvisory::analyze_book_instability,        // idx 13
    };
    static_assert(std::size(analyzers) == 14, "cooldown_tracker_ size mismatch");

    for (int idx = 0; idx < static_cast<int>(std::size(analyzers)); ++idx) {
        // Проверка deadline перед каждым детектором
        if (std::chrono::steady_clock::now() >= deadline) {
            if (logger_) {
                logger_->warn("AIAdvisory", "Deadline hit — оставшиеся детекторы пропущены",
                    {{"detector_idx", std::to_string(idx)},
                     {"timeout_ms", std::to_string(config_.timeout_ms)}});
            }
            break;
        }

        auto adv = (this->*analyzers[idx])(snapshot);
        if (!adv) continue;

        // Per-detector cooldown (точнее, чем per-type)
        if (is_on_cooldown(idx, now_ns)) continue;
        update_cooldown(idx, now_ns);

        adv->generated_at = snapshot.computed_at;
        adv->is_available = true;

        if (adv->severity > result.max_severity)
            result.max_severity = adv->severity;

        if (adv->type == AIRecommendationType::AnomalyWarning && metric_anomaly_warnings_)
            metric_anomaly_warnings_->increment();
        else if (adv->type == AIRecommendationType::RiskWarning && metric_risk_warnings_)
            metric_risk_warnings_->increment();
        else if (adv->type == AIRecommendationType::StrategyHint && metric_strategy_hints_)
            metric_strategy_hints_->increment();

        if (logger_) {
            logger_->debug("AIAdvisory", adv->insight,
                {{"detector", std::to_string(idx)},
                 {"type", to_string(adv->type)},
                 {"confidence", to_string(adv->confidence_level)},
                 {"adjustment", std::to_string(adv->confidence_adjustment)},
                 {"severity", std::to_string(adv->severity)}});
        }

        result.advisories.push_back(std::move(*adv));
    }

    // Сортируем по severity убыванию
    std::sort(result.advisories.begin(), result.advisories.end(),
              [](const auto& a, const auto& b) { return a.severity > b.severity; });

    result.ensemble_count = static_cast<int>(result.advisories.size());

    // --- Ансамблевая эскалация ---
    // Если N+ детекторов сработали одновременно → реальная ситуация опаснее,
    // чем наивысший одиночный сигнал: прибавляем бонус к max_severity.
    if (result.ensemble_count >= config_.ensemble_escalation_count) {
        result.max_severity = std::min(1.0,
            result.max_severity + config_.ensemble_escalation_bonus);
        if (logger_) {
            logger_->info("AIAdvisory", "Ансамблевая эскалация severity",
                {{"detectors_fired", std::to_string(result.ensemble_count)},
                 {"escalated_severity", std::to_string(result.max_severity)},
                 {"bonus", std::to_string(config_.ensemble_escalation_bonus)}});
        }
    }

    // --- Severity-weighted confidence adjustment ---
    // Взвешиваем по severity и уровню уверенности.
    // Confidence weights: High=1.5, Medium=1.0, Low=0.7
    if (!result.advisories.empty()) {
        double weighted_sum = 0.0;
        double weight_total = 0.0;
        for (const auto& adv : result.advisories) {
            double conf_w = (adv.confidence_level == AIConfidenceLevel::High) ? 1.5 :
                            (adv.confidence_level == AIConfidenceLevel::Medium) ? 1.0 : 0.7;
            double w = adv.severity * conf_w;
            if (config_.use_severity_weighted_adjustment) {
                weighted_sum += adv.confidence_adjustment * w;
                weight_total += w;
            } else {
                weighted_sum += adv.confidence_adjustment;
                weight_total += 1.0;
            }
        }
        double raw_adj = (weight_total > 0.0) ? (weighted_sum / weight_total) : 0.0;
        result.total_confidence_adjustment = clamp_adjustment(raw_adj);
    }

    result.has_veto = result.max_severity >= config_.veto_severity_threshold;

    // --- Гистерезис ---
    update_advisory_state(result);

    // Timing
    auto elapsed = std::chrono::steady_clock::now() - start;
    result.processing_time_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(elapsed).count();
    for (auto& adv : result.advisories) {
        adv.processing_time_ms = result.processing_time_ms;
    }

    // Update status & metrics
    if (!result.advisories.empty()) {
        status_.advisories_generated += static_cast<int>(result.advisories.size());
        if (metric_hits_) metric_hits_->increment(static_cast<double>(result.advisories.size()));
    }
    if (metric_processing_ms_) metric_processing_ms_->observe(
        static_cast<double>(result.processing_time_ms));
    if (metric_confidence_adj_) metric_confidence_adj_->set(result.total_confidence_adjustment);

    status_.last_success = snapshot.computed_at;

    if (logger_ && !result.advisories.empty()) {
        logger_->info("AIAdvisory", "Рекомендации сгенерированы",
            {{"count", std::to_string(result.advisories.size())},
             {"total_adj", std::to_string(result.total_confidence_adjustment)},
             {"max_severity", std::to_string(result.max_severity)},
             {"has_veto", result.has_veto ? "true" : "false"},
             {"state", to_string(result.advisory_state)},
             {"size_mult", std::to_string(result.advisory_size_multiplier)},
             {"processing_ms", std::to_string(result.processing_time_ms)}});
    }

    return result;
}

void LocalRuleBasedAdvisory::update_advisory_state(AIAdvisoryResult& result) {
    // Машина состояний с гистерезисом предотвращает whipsaw на границах порогов.
    //
    // Переходы:
    //   Clear   → Caution: max_severity >= caution_threshold
    //   Clear   → Veto:    max_severity >= veto_threshold
    //   Caution → Veto:    max_severity >= veto_threshold
    //   Veto    → Caution: max_severity < veto_threshold (немедленно, но не Clear)
    //   Caution → Clear:   hysteresis_ticks_below_ >= hysteresis_clear_ticks
    //   Veto    → Clear:   через Caution (двухступенчатый выход)

    double sev = result.max_severity;
    bool above_veto   = sev >= config_.veto_severity_threshold;
    bool above_caution = sev >= config_.caution_severity_threshold;

    if (above_veto) {
        advisory_state_ = AdvisoryState::Veto;
        hysteresis_ticks_below_ = 0;
    } else if (above_caution) {
        // Понижение из Veto в Caution — немедленно
        advisory_state_ = AdvisoryState::Caution;
        hysteresis_ticks_below_ = 0;
    } else {
        // Ниже caution-порога — считаем тики для сброса
        ++hysteresis_ticks_below_;
        if (hysteresis_ticks_below_ >= config_.hysteresis_clear_ticks) {
            advisory_state_ = AdvisoryState::Clear;
        }
        // Иначе держим предыдущее состояние (Caution или Clear)
    }

    result.advisory_state = advisory_state_;

    // Устанавливаем size_multiplier в зависимости от состояния
    switch (advisory_state_) {
        case AdvisoryState::Veto:
            result.has_veto = true;
            result.advisory_size_multiplier = 0.0;
            break;
        case AdvisoryState::Caution:
            result.advisory_size_multiplier = config_.caution_size_multiplier;
            break;
        case AdvisoryState::Clear:
            result.advisory_size_multiplier = 1.0;
            break;
    }
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::get_advisory(
    const features::FeatureSnapshot& snapshot)
{
    auto result = get_advisories(snapshot);
    if (result.empty()) return std::nullopt;
    return result.advisories.front();  // Top-1 by severity
}

bool LocalRuleBasedAdvisory::is_available() const { return config_.enabled; }

AIServiceStatus LocalRuleBasedAdvisory::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

// ============================================================
// Original 4 detectors (refactored with configurable thresholds)
// ============================================================

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_extreme_volatility(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.volatility_valid) return std::nullopt;
    if (s.technical.volatility_20 <= 0.0) return std::nullopt;

    double ratio = s.technical.volatility_5 / s.technical.volatility_20;
    if (ratio <= config_.thresholds.volatility_ratio_threshold) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::AnomalyWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = config_.thresholds.volatility_confidence_adj;
    adv.severity = std::min(1.0, ratio / (config_.thresholds.volatility_ratio_threshold * 2.0));
    adv.insight = "Экстремальный всплеск краткосрочной волатильности";
    adv.anomaly_explanation = "Vol5=" + std::to_string(s.technical.volatility_5) +
        " превышает " + std::to_string(config_.thresholds.volatility_ratio_threshold) +
        "x Vol20=" + std::to_string(s.technical.volatility_20);
    adv.tags = {"volatility", "extreme", "caution"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_rsi_extreme(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.rsi_valid) return std::nullopt;
    double rsi = s.technical.rsi_14;

    if (rsi > config_.thresholds.rsi_overbought) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::StrategyHint;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = config_.thresholds.rsi_confidence_adj;
        adv.severity = std::min(1.0, (rsi - config_.thresholds.rsi_overbought) / 15.0);
        adv.insight = "RSI в зоне экстремальной перекупленности (" + std::to_string(static_cast<int>(rsi)) + ")";
        adv.regime_suggestion = "Возможен откат";
        adv.tags = {"rsi", "overbought"};
        return adv;
    }
    if (rsi < config_.thresholds.rsi_oversold) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::StrategyHint;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = config_.thresholds.rsi_confidence_adj;
        adv.severity = std::min(1.0, (config_.thresholds.rsi_oversold - rsi) / 15.0);
        adv.insight = "RSI в зоне экстремальной перепроданности (" + std::to_string(static_cast<int>(rsi)) + ")";
        adv.regime_suggestion = "Возможен отскок";
        adv.tags = {"rsi", "oversold"};
        return adv;
    }
    return std::nullopt;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_spread_anomaly(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.spread_valid) return std::nullopt;
    if (s.microstructure.spread_bps <= config_.thresholds.spread_anomaly_bps) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = config_.thresholds.spread_confidence_adj;
    adv.severity = std::min(1.0, s.microstructure.spread_bps / (config_.thresholds.spread_anomaly_bps * 4.0));
    adv.insight = "Аномально широкий спред: " + std::to_string(static_cast<int>(s.microstructure.spread_bps)) + " bps";
    adv.anomaly_explanation = "Возможна низкая ликвидность или рыночный стресс";
    adv.tags = {"spread", "anomaly"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_liquidity_concern(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.liquidity_valid) return std::nullopt;
    if (s.microstructure.liquidity_ratio >= config_.thresholds.liquidity_ratio_min) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = config_.thresholds.liquidity_confidence_adj;
    adv.severity = std::min(1.0, 1.0 - (s.microstructure.liquidity_ratio / config_.thresholds.liquidity_ratio_min));
    adv.insight = "Низкий коэффициент ликвидности: " + std::to_string(s.microstructure.liquidity_ratio);
    adv.anomaly_explanation = "Асимметрия ликвидности — повышенный риск проскальзывания";
    adv.tags = {"liquidity", "imbalance"};
    return adv;
}

// ============================================================
// New detectors (10 additional)
// ============================================================

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_vpin_toxic_flow(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.vpin_valid) return std::nullopt;
    if (!s.microstructure.vpin_toxic && s.microstructure.vpin <= config_.thresholds.vpin_toxic_threshold)
        return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = config_.thresholds.vpin_confidence_adj;
    adv.severity = std::min(1.0, s.microstructure.vpin / 1.0);
    adv.insight = "VPIN токсичный поток обнаружен: " + std::to_string(s.microstructure.vpin);
    adv.anomaly_explanation = "Вероятность информированной торговли повышена (VPIN=" +
        std::to_string(s.microstructure.vpin) + ", MA=" + std::to_string(s.microstructure.vpin_ma) + ")";
    adv.tags = {"vpin", "toxic_flow", "risk"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_volume_profile_anomaly(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.vp_valid) return std::nullopt;
    double deviation = std::abs(s.technical.vp_price_vs_poc);
    if (deviation <= config_.thresholds.vp_poc_deviation_threshold) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RegimeInsight;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = config_.thresholds.vp_confidence_adj;
    adv.severity = std::min(0.6, deviation / 1.0);
    adv.insight = "Цена значительно отклонилась от POC (Volume Profile)";
    adv.anomaly_explanation = "price_vs_poc=" + std::to_string(s.technical.vp_price_vs_poc) +
        ", POC=" + std::to_string(s.technical.vp_poc);
    adv.regime_suggestion = s.technical.vp_price_vs_poc > 0 ? "Цена выше зоны стоимости" : "Цена ниже зоны стоимости";
    adv.tags = {"volume_profile", "poc_deviation"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_macd_divergence(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.macd_valid || !s.technical.momentum_valid) return std::nullopt;

    // Divergence: MACD histogram and momentum disagree on direction
    bool macd_bullish = s.technical.macd_histogram > 0.0;
    bool momentum_bullish = s.technical.momentum_5 > 0.0;
    if (macd_bullish == momentum_bullish) return std::nullopt;

    // Only flag if MACD histogram is significant
    if (std::abs(s.technical.macd_histogram) < 0.0001) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::StrategyHint;
    adv.confidence_level = AIConfidenceLevel::Low;
    adv.confidence_adjustment = config_.thresholds.macd_confidence_adj;
    adv.severity = 0.3;
    adv.insight = "MACD/Momentum дивергенция обнаружена";
    adv.anomaly_explanation = "MACD_hist=" + std::to_string(s.technical.macd_histogram) +
        " vs momentum_5=" + std::to_string(s.technical.momentum_5);
    adv.regime_suggestion = "Возможна смена направления";
    adv.tags = {"macd", "divergence", "momentum"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_bollinger_squeeze(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.bb_valid) return std::nullopt;

    // Squeeze detection: very low bandwidth
    if (s.technical.bb_bandwidth < config_.thresholds.bb_squeeze_bandwidth) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::RegimeInsight;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = config_.thresholds.bb_confidence_adj;
        adv.severity = 0.4;
        adv.insight = "Bollinger Band сжатие — ожидается пробой";
        adv.anomaly_explanation = "BB_bandwidth=" + std::to_string(s.technical.bb_bandwidth) +
            " ниже порога " + std::to_string(config_.thresholds.bb_squeeze_bandwidth);
        adv.regime_suggestion = "Возможен сильный импульсный выход из диапазона";
        adv.tags = {"bollinger", "squeeze", "breakout_imminent"};
        return adv;
    }

    // Breakout detection: %B outside bands
    if (s.technical.bb_percent_b > config_.thresholds.bb_breakout_percent_b_high ||
        s.technical.bb_percent_b < config_.thresholds.bb_breakout_percent_b_low) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::AnomalyWarning;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = config_.thresholds.bb_confidence_adj;
        adv.severity = 0.5;
        adv.insight = "Bollinger Band пробой: %B=" + std::to_string(s.technical.bb_percent_b);
        adv.anomaly_explanation = "Цена вышла за пределы полос Боллинджера";
        adv.tags = {"bollinger", "breakout"};
        return adv;
    }

    return std::nullopt;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_adx_trend_strength(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.adx_valid) return std::nullopt;

    if (s.technical.adx > config_.thresholds.adx_strong_trend) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::RegimeInsight;
        adv.confidence_level = AIConfidenceLevel::High;
        adv.confidence_adjustment = config_.thresholds.adx_confidence_adj;
        adv.severity = 0.4;
        adv.insight = "Экстремально сильный тренд (ADX=" + std::to_string(static_cast<int>(s.technical.adx)) + ")";
        adv.regime_suggestion = s.technical.plus_di > s.technical.minus_di ? "Сильный аптренд" : "Сильный даунтренд";
        adv.tags = {"adx", "strong_trend"};
        return adv;
    }

    if (s.technical.adx < config_.thresholds.adx_no_trend) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::RegimeInsight;
        adv.confidence_level = AIConfidenceLevel::Low;
        adv.confidence_adjustment = config_.thresholds.adx_confidence_adj * 0.5; // Less severe
        adv.severity = 0.2;
        adv.insight = "Отсутствие тренда (ADX=" + std::to_string(static_cast<int>(s.technical.adx)) + ")";
        adv.regime_suggestion = "Рынок в боковике — трендовые стратегии неэффективны";
        adv.tags = {"adx", "no_trend", "ranging"};
        return adv;
    }

    return std::nullopt;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_book_imbalance(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.book_imbalance_valid) return std::nullopt;

    double imbalance = std::abs(s.microstructure.book_imbalance_5);
    if (imbalance <= config_.thresholds.book_imbalance_threshold) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::AnomalyWarning;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = config_.thresholds.book_imbalance_confidence_adj;
    adv.severity = std::min(0.7, imbalance);
    adv.insight = "Сильный перекос стакана: imbalance=" +
        std::to_string(s.microstructure.book_imbalance_5);
    adv.anomaly_explanation = s.microstructure.book_imbalance_5 > 0
        ? "Доминирование покупателей (давление на bid)" : "Доминирование продавцов (давление на ask)";
    adv.tags = {"book_imbalance", "microstructure"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_time_of_day_risk(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.tod_valid) return std::nullopt;
    if (s.technical.tod_alpha_score >= config_.thresholds.tod_low_alpha_threshold) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::ConfidenceAdjust;
    adv.confidence_level = AIConfidenceLevel::Low;
    adv.confidence_adjustment = config_.thresholds.tod_confidence_adj;
    adv.severity = 0.2;
    adv.insight = "Неблагоприятное время для торговли (alpha_score=" +
        std::to_string(s.technical.tod_alpha_score) + ", hour=" +
        std::to_string(s.technical.session_hour_utc) + " UTC)";
    adv.anomaly_explanation = "Низкая историческая альфа в текущий час";
    adv.tags = {"time_of_day", "low_alpha"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_cusum_regime_change(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.cusum_valid) return std::nullopt;
    if (!s.technical.cusum_regime_change) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::AnomalyWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = config_.thresholds.cusum_confidence_adj;
    adv.severity = 0.7;
    adv.insight = "CUSUM обнаружил смену режима рынка";
    adv.anomaly_explanation = "CUSUM+=" + std::to_string(s.technical.cusum_positive) +
        ", CUSUM-=" + std::to_string(s.technical.cusum_negative) +
        ", threshold=" + std::to_string(s.technical.cusum_threshold);
    adv.regime_suggestion = "Структурная смена режима — рекомендуется пересмотр параметров";
    adv.tags = {"cusum", "regime_change", "structural"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_momentum_divergence(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.momentum_valid) return std::nullopt;

    // Check if short-term and long-term momentum diverge significantly
    double divergence = std::abs(s.technical.momentum_5 - s.technical.momentum_20);
    if (divergence <= config_.thresholds.momentum_divergence_threshold) return std::nullopt;

    // Opposing directions matter more
    bool opposing = (s.technical.momentum_5 > 0) != (s.technical.momentum_20 > 0);
    if (!opposing) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::StrategyHint;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = config_.thresholds.momentum_confidence_adj;
    adv.severity = std::min(0.5, divergence);
    adv.insight = "Дивергенция моментумов: mom5=" + std::to_string(s.technical.momentum_5) +
        " vs mom20=" + std::to_string(s.technical.momentum_20);
    adv.regime_suggestion = "Краткосрочный и долгосрочный моментум расходятся — осторожность";
    adv.tags = {"momentum", "divergence"};
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_book_instability(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.instability_valid) return std::nullopt;
    if (s.microstructure.book_instability <= config_.thresholds.book_instability_threshold)
        return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = config_.thresholds.book_instability_confidence_adj;
    adv.severity = std::min(0.8, s.microstructure.book_instability);
    adv.insight = "Нестабильный стакан: instability=" +
        std::to_string(s.microstructure.book_instability);
    adv.anomaly_explanation = "Высокая частота изменений в стакане ордеров — риск проскальзывания";
    adv.tags = {"book_instability", "microstructure", "risk"};
    return adv;
}

} // namespace tb::ai
