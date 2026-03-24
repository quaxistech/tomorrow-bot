/**
 * @file ai_advisory_engine.cpp
 * @brief Реализация локального AI Advisory на основе правил
 */
#include "ai/ai_advisory_engine.hpp"
#include <chrono>
#include <cmath>

namespace tb::ai {

LocalRuleBasedAdvisory::LocalRuleBasedAdvisory(
    AIAdvisoryConfig config, std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config)), logger_(std::move(logger))
{
    status_.available = config_.enabled;
    status_.healthy = config_.enabled;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::get_advisory(
    const features::FeatureSnapshot& snapshot)
{
    if (!config_.enabled) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    status_.requests_total++;
    auto start = std::chrono::steady_clock::now();

    // Проверяем правила, возвращаем первое сработавшее
    using AnalyzeFn = std::optional<AIAdvisory> (LocalRuleBasedAdvisory::*)(
        const features::FeatureSnapshot&) const;
    for (AnalyzeFn analyzer : {
        &LocalRuleBasedAdvisory::analyze_extreme_volatility,
        &LocalRuleBasedAdvisory::analyze_rsi_extreme,
        &LocalRuleBasedAdvisory::analyze_spread_anomaly,
        &LocalRuleBasedAdvisory::analyze_liquidity_concern
    }) {
        if (auto adv = (this->*analyzer)(snapshot)) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            adv->processing_time_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(elapsed).count();
            adv->is_available = true;
            status_.last_success = snapshot.computed_at;
            return adv;
        }
    }

    status_.last_success = snapshot.computed_at;
    return std::nullopt;
}

bool LocalRuleBasedAdvisory::is_available() const { return config_.enabled; }

AIServiceStatus LocalRuleBasedAdvisory::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_extreme_volatility(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.volatility_valid) return std::nullopt;
    if (s.technical.volatility_20 <= 0.0) return std::nullopt;
    if (s.technical.volatility_5 <= s.technical.volatility_20 * 3.0) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::AnomalyWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = -0.3;
    adv.insight = "Экстремальный всплеск краткосрочной волатильности";
    adv.anomaly_explanation = "Vol5=" + std::to_string(s.technical.volatility_5) +
        " превышает 3x Vol20=" + std::to_string(s.technical.volatility_20);
    adv.tags = {"volatility", "extreme", "caution"};
    adv.generated_at = s.computed_at;
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_rsi_extreme(
    const features::FeatureSnapshot& s) const
{
    if (!s.technical.rsi_valid) return std::nullopt;
    double rsi = s.technical.rsi_14;

    if (rsi > 85.0) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::StrategyHint;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = -0.15;
        adv.insight = "RSI в зоне экстремальной перекупленности (" + std::to_string(int(rsi)) + ")";
        adv.regime_suggestion = "Возможен откат";
        adv.tags = {"rsi", "overbought"};
        adv.generated_at = s.computed_at;
        return adv;
    }
    if (rsi < 15.0) {
        AIAdvisory adv;
        adv.type = AIRecommendationType::StrategyHint;
        adv.confidence_level = AIConfidenceLevel::Medium;
        adv.confidence_adjustment = -0.15;
        adv.insight = "RSI в зоне экстремальной перепроданности (" + std::to_string(int(rsi)) + ")";
        adv.regime_suggestion = "Возможен отскок";
        adv.tags = {"rsi", "oversold"};
        adv.generated_at = s.computed_at;
        return adv;
    }
    return std::nullopt;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_spread_anomaly(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.spread_valid) return std::nullopt;
    if (s.microstructure.spread_bps <= 50.0) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::High;
    adv.confidence_adjustment = -0.25;
    adv.insight = "Аномально широкий спред: " + std::to_string(int(s.microstructure.spread_bps)) + " bps";
    adv.anomaly_explanation = "Возможна низкая ликвидность или рыночный стресс";
    adv.tags = {"spread", "anomaly"};
    adv.generated_at = s.computed_at;
    return adv;
}

std::optional<AIAdvisory> LocalRuleBasedAdvisory::analyze_liquidity_concern(
    const features::FeatureSnapshot& s) const
{
    if (!s.microstructure.liquidity_valid) return std::nullopt;
    if (s.microstructure.liquidity_ratio >= 0.3) return std::nullopt;

    AIAdvisory adv;
    adv.type = AIRecommendationType::RiskWarning;
    adv.confidence_level = AIConfidenceLevel::Medium;
    adv.confidence_adjustment = -0.2;
    adv.insight = "Низкий коэффициент ликвидности: " + std::to_string(s.microstructure.liquidity_ratio);
    adv.anomaly_explanation = "Асимметрия ликвидности — повышенный риск проскальзывания";
    adv.tags = {"liquidity", "imbalance"};
    adv.generated_at = s.computed_at;
    return adv;
}

} // namespace tb::ai
