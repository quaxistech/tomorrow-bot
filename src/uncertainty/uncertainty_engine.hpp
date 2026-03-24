#pragma once
#include "uncertainty/uncertainty_types.hpp"
#include "features/feature_snapshot.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tb::uncertainty {

/// Интерфейс движка оценки неопределённости
class IUncertaintyEngine {
public:
    virtual ~IUncertaintyEngine() = default;

    /// Оценка неопределённости на основе всех доступных данных
    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) = 0;

    /// Текущая оценка неопределённости для символа
    virtual std::optional<UncertaintySnapshot> current(const Symbol& symbol) const = 0;
};

/// Реализация оценки неопределённости на основе правил
class RuleBasedUncertaintyEngine : public IUncertaintyEngine {
public:
    RuleBasedUncertaintyEngine(std::shared_ptr<logging::ILogger> logger,
                                std::shared_ptr<clock::IClock> clock);

    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) override;

    std::optional<UncertaintySnapshot> current(const Symbol& symbol) const override;

private:
    /// Неопределённость режима: 1.0 - confidence
    double compute_regime_uncertainty(const regime::RegimeSnapshot& regime) const;

    /// Неопределённость сигналов: конфликтующие индикаторы
    double compute_signal_uncertainty(const features::FeatureSnapshot& features) const;

    /// Неопределённость качества данных
    double compute_data_quality_uncertainty(const features::FeatureSnapshot& features) const;

    /// Неопределённость исполнения
    double compute_execution_uncertainty(const features::FeatureSnapshot& features) const;

    /// Агрегация размерностей в общий скор
    double aggregate(const UncertaintyDimensions& dims) const;

    /// Маппинг скора на уровень
    UncertaintyLevel score_to_level(double score) const;

    /// Маппинг уровня на рекомендацию
    UncertaintyAction level_to_action(UncertaintyLevel level) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::unordered_map<std::string, UncertaintySnapshot> snapshots_;
    mutable std::mutex mutex_;
};

} // namespace tb::uncertainty
