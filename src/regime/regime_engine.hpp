#pragma once
#include "regime/regime_types.hpp"
#include "regime/regime_config.hpp"
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

/// Классификатор режима на основе правил с конфигурируемыми порогами и hysteresis
class RuleBasedRegimeEngine : public IRegimeEngine {
public:
    RuleBasedRegimeEngine(std::shared_ptr<logging::ILogger> logger,
                          std::shared_ptr<clock::IClock> clock,
                          std::shared_ptr<metrics::IMetricsRegistry> metrics,
                          RegimeConfig config = make_default_regime_config());

    RegimeSnapshot classify(const features::FeatureSnapshot& snapshot) override;
    std::optional<RegimeSnapshot> current_regime(const Symbol& symbol) const override;

private:
    /// Per-symbol hysteresis state
    struct HysteresisState {
        DetailedRegime confirmed_regime{DetailedRegime::Undefined};
        DetailedRegime candidate_regime{DetailedRegime::Undefined};
        int candidate_ticks{0};  ///< How many ticks candidate has persisted
        int dwell_ticks{0};      ///< How many ticks since last confirmed switch
    };

    /// Мгновенная rule-based классификация (без hysteresis)
    DetailedRegime classify_immediate(const features::FeatureSnapshot& snap,
                                      ClassificationExplanation& explanation) const;

    /// Применить hysteresis к мгновенному результату
    DetailedRegime apply_hysteresis(DetailedRegime immediate, double confidence,
                                    HysteresisState& state,
                                    ClassificationExplanation& explanation) const;

    double compute_confidence(const features::FeatureSnapshot& snap,
                              DetailedRegime regime,
                              const ClassificationExplanation& explanation) const;

    double compute_stability(DetailedRegime current, DetailedRegime previous) const;

    std::vector<RegimeStrategyHint> generate_hints(DetailedRegime regime) const;

    /// Формирование человекочитаемого резюме
    static std::string build_summary(DetailedRegime regime, double confidence,
                                     double stability, bool hysteresis_overrode);

    RegimeConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::unordered_map<std::string, RegimeSnapshot> snapshots_;
    std::unordered_map<std::string, HysteresisState> hysteresis_;
    mutable std::mutex mutex_;
};

} // namespace tb::regime
