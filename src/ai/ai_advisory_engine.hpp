#pragma once
/**
 * @file ai_advisory_engine.hpp
 * @brief AI Advisory движок — интерфейс и локальная реализация
 *
 * Работает асинхронно с таймаутом. Не блокирует критический путь.
 * Не может обойти правила риска. Fallback: nullopt.
 */
#include "ai/ai_advisory_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>

namespace tb::ai {

/// Абстрактный интерфейс AI Advisory
class IAIAdvisoryEngine {
public:
    virtual ~IAIAdvisoryEngine() = default;
    virtual std::optional<AIAdvisory> get_advisory(
        const features::FeatureSnapshot& snapshot) = 0;
    [[nodiscard]] virtual bool is_available() const = 0;
    [[nodiscard]] virtual AIServiceStatus get_status() const = 0;
};

/**
 * @brief Локальная реализация — правиловый анализ без внешних ML-сервисов
 *
 * Анализирует: экстремальную волатильность, RSI, аномальный спред, ликвидность.
 * Заменяема на ML/LLM реализацию без изменения интерфейса.
 */
class LocalRuleBasedAdvisory : public IAIAdvisoryEngine {
public:
    explicit LocalRuleBasedAdvisory(
        AIAdvisoryConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    std::optional<AIAdvisory> get_advisory(
        const features::FeatureSnapshot& snapshot) override;
    [[nodiscard]] bool is_available() const override;
    [[nodiscard]] AIServiceStatus get_status() const override;

private:
    AIAdvisoryConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    mutable std::mutex mutex_;
    mutable AIServiceStatus status_;

    std::optional<AIAdvisory> analyze_extreme_volatility(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_rsi_extreme(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_spread_anomaly(const features::FeatureSnapshot& s) const;
    std::optional<AIAdvisory> analyze_liquidity_concern(const features::FeatureSnapshot& s) const;
};

} // namespace tb::ai
