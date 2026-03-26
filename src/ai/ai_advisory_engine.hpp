#pragma once
/**
 * @file ai_advisory_engine.hpp
 * @brief AI Advisory движок — интерфейс и локальная реализация
 *
 * Работает синхронно в горячем пути. Не блокирует выше timeout_ms (жёсткий deadline).
 * Не может обойти правила риска. Fallback: пустой результат.
 *
 * Архитектура:
 * - IAIAdvisoryEngine: абстрактный интерфейс (plugin-ready для ML/LLM)
 * - LocalRuleBasedAdvisory: 14 детекторов на основе правил
 * - get_advisories(): возвращает ВСЕ сработавшие рекомендации
 * - get_advisory(): backward-compat, возвращает top-1 по severity
 *
 * Профессиональные улучшения (v2):
 * - AdvisoryState: Clear / Caution / Veto с гистерезисом (hysteresis_clear_ticks)
 * - Ансамблевая эскалация: N детекторов → +ensemble_escalation_bonus к severity
 * - Severity-weighted aggregation confidence_adjustment
 * - Жёсткий deadline: timeout_ms отсекает зависшие детекторы
 * - caution_size_multiplier: при Caution размер позиции умножается (не блокируется)
 */
#include "ai/ai_advisory_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace tb::ai {

/// Абстрактный интерфейс AI Advisory
class IAIAdvisoryEngine {
public:
    virtual ~IAIAdvisoryEngine() = default;

    /// Получить все рекомендации для текущего снапшота
    virtual AIAdvisoryResult get_advisories(
        const features::FeatureSnapshot& snapshot) = 0;

    /// Backward-compat: вернуть top-1 рекомендацию (или nullopt)
    virtual std::optional<AIAdvisory> get_advisory(
        const features::FeatureSnapshot& snapshot) = 0;

    [[nodiscard]] virtual bool is_available() const = 0;
    [[nodiscard]] virtual AIServiceStatus get_status() const = 0;
};

/**
 * @brief Локальная реализация — правиловый анализ без внешних ML-сервисов
 *
 * 14 детекторов: волатильность, RSI, спред, ликвидность, VPIN,
 * Volume Profile, MACD, Bollinger Bands, ADX, Book Imbalance,
 * Time-of-Day, CUSUM, Momentum, Book Instability.
 *
 * Заменяема на ML/LLM реализацию без изменения интерфейса.
 */
class LocalRuleBasedAdvisory : public IAIAdvisoryEngine {
public:
    explicit LocalRuleBasedAdvisory(
        AIAdvisoryConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    AIAdvisoryResult get_advisories(
        const features::FeatureSnapshot& snapshot) override;

    std::optional<AIAdvisory> get_advisory(
        const features::FeatureSnapshot& snapshot) override;

    [[nodiscard]] bool is_available() const override;
    [[nodiscard]] AIServiceStatus get_status() const override;

private:
    AIAdvisoryConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    mutable std::mutex mutex_;
    mutable AIServiceStatus status_;

    // Cooldown tracking: detector-index → last trigger timestamp (nanoseconds)
    // Используем индекс детектора (0-13), а не тип (5 значений) — точнее на 14 детекторах
    std::array<int64_t, 14> cooldown_tracker_{};

    // Hysteresis state machine
    AdvisoryState advisory_state_{AdvisoryState::Clear};  ///< Текущее устойчивое состояние
    int hysteresis_ticks_below_{0};                       ///< Тиков подряд ниже caution-порога

    // Metrics handles (initialized once)
    std::shared_ptr<metrics::ICounter> metric_requests_;
    std::shared_ptr<metrics::ICounter> metric_hits_;
    std::shared_ptr<metrics::ICounter> metric_anomaly_warnings_;
    std::shared_ptr<metrics::ICounter> metric_risk_warnings_;
    std::shared_ptr<metrics::ICounter> metric_strategy_hints_;
    std::shared_ptr<metrics::IHistogram> metric_processing_ms_;
    std::shared_ptr<metrics::IGauge> metric_confidence_adj_;

    void init_metrics();

    /// Check cooldown for a detector by index (0-13)
    bool is_on_cooldown(int detector_idx, int64_t now_ns) const;
    void update_cooldown(int detector_idx, int64_t now_ns);

    /// Update hysteresis state machine and set result fields accordingly
    void update_advisory_state(AIAdvisoryResult& result);

    /// Clamp aggregated confidence adjustment to config limits
    double clamp_adjustment(double adj) const;

    // ===== Detectors (original 4) =====
    std::optional<AIAdvisory> analyze_extreme_volatility(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_rsi_extreme(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_spread_anomaly(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_liquidity_concern(const features::FeatureSnapshot& s) const;

    // ===== New detectors (Phase 3) =====
    std::optional<AIAdvisory> analyze_vpin_toxic_flow(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_volume_profile_anomaly(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_macd_divergence(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_bollinger_squeeze(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_adx_trend_strength(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_book_imbalance(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_time_of_day_risk(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_cusum_regime_change(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_momentum_divergence(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_book_instability(const features::FeatureSnapshot& s) const;
};

} // namespace tb::ai
