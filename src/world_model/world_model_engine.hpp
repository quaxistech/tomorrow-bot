#pragma once
#include "world_model/world_model_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tb::world_model {

/// Интерфейс движка мировой модели
class IWorldModelEngine {
public:
    virtual ~IWorldModelEngine() = default;

    /// Обновляет мировую модель на основе нового снимка признаков
    virtual WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) = 0;

    /// Текущее состояние мировой модели для символа
    virtual std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const = 0;
};

/// Реализация мировой модели на основе правил (эвристическая)
/// В будущем можно заменить на ML-модель без изменения интерфейса
class RuleBasedWorldModelEngine : public IWorldModelEngine {
public:
    explicit RuleBasedWorldModelEngine(std::shared_ptr<logging::ILogger> logger,
                                       std::shared_ptr<clock::IClock> clock);

    WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) override;
    std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const override;

private:
    /// Классифицирует состояние мира по признакам
    WorldState classify_state(const features::FeatureSnapshot& snap) const;

    /// Оценивает хрупкость текущего состояния
    FragilityScore compute_fragility(const features::FeatureSnapshot& snap, WorldState state) const;

    /// Определяет тенденцию перехода (сравнивает с предыдущим состоянием)
    TransitionTendency compute_tendency(WorldState current, WorldState previous) const;

    /// Вычисляет пригодность состояния для каждой стратегии
    std::vector<StrategySuitability> compute_suitability(WorldState state,
                                                         const features::FeatureSnapshot& snap) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::unordered_map<std::string, WorldModelSnapshot> states_; ///< По символу
    mutable std::mutex mutex_;
};

} // namespace tb::world_model
