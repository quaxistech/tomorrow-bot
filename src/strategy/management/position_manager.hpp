#pragma once
/**
 * @file position_manager.hpp
 * @brief Управление позициями: сопровождение, выход, reduce (§17-18 ТЗ)
 */

#include "strategy/strategy_types.hpp"
#include "strategy/strategy_config.hpp"
#include "strategy/state/setup_models.hpp"
#include "features/feature_snapshot.hpp"

namespace tb::strategy {

/// Менеджер сопровождения позиции и выхода
class PositionManager {
public:
    explicit PositionManager(const ScalpStrategyConfig& cfg) : cfg_(cfg) {}

    /// Оценить позицию и принять решение hold/reduce/exit
    PositionManagementResult evaluate(const StrategyPositionContext& pos,
                                      const StrategyContext& ctx,
                                      int64_t now_ns) const;

private:
    /// Проверка time stop
    bool check_time_stop(const StrategyPositionContext& pos, int64_t now_ns) const;

    /// Проверка структурного провала
    bool check_structure_failure(const StrategyPositionContext& pos,
                                 const StrategyContext& ctx) const;

    /// Проверка достижения цели
    bool check_target_reached(const StrategyPositionContext& pos,
                              const StrategyContext& ctx) const;

    /// Проверка деградации качества
    bool check_quality_degradation(const StrategyPositionContext& pos,
                                   const StrategyContext& ctx) const;

    const ScalpStrategyConfig& cfg_;
};

} // namespace tb::strategy
