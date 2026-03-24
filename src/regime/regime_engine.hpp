#pragma once
#include "regime/regime_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tb::regime {

/// Интерфейс движка классификации рыночных режимов
class IRegimeEngine {
public:
    virtual ~IRegimeEngine() = default;

    /// Классифицирует режим рынка на основе снимка признаков
    virtual RegimeSnapshot classify(const features::FeatureSnapshot& snapshot) = 0;

    /// Текущий режим для символа
    virtual std::optional<RegimeSnapshot> current_regime(const Symbol& symbol) const = 0;
};

/// Классификатор режима на основе правил
class RuleBasedRegimeEngine : public IRegimeEngine {
public:
    RuleBasedRegimeEngine(std::shared_ptr<logging::ILogger> logger,
                          std::shared_ptr<clock::IClock> clock,
                          std::shared_ptr<metrics::IMetricsRegistry> metrics);

    RegimeSnapshot classify(const features::FeatureSnapshot& snapshot) override;
    std::optional<RegimeSnapshot> current_regime(const Symbol& symbol) const override;

private:
    /// Детализированная классификация режима
    DetailedRegime classify_detailed(const features::FeatureSnapshot& snap) const;

    /// Вычисление уверенности в классификации
    double compute_confidence(const features::FeatureSnapshot& snap, DetailedRegime regime) const;

    /// Вычисление стабильности (сравнение с предыдущим)
    double compute_stability(DetailedRegime current, DetailedRegime previous) const;

    /// Генерация рекомендаций по стратегиям
    std::vector<RegimeStrategyHint> generate_hints(DetailedRegime regime) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::unordered_map<std::string, RegimeSnapshot> snapshots_;
    mutable std::mutex mutex_;
};

} // namespace tb::regime
