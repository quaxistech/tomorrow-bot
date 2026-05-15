#pragma once
#include "strategy_allocator/allocation_types.hpp"
#include "strategy/strategy_interface.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <vector>

namespace tb::strategy_allocator {

/// Интерфейс аллокатора стратегий
class IStrategyAllocator {
public:
    virtual ~IStrategyAllocator() = default;

    /// Рассчитать аллокацию на основе текущего контекста
    virtual AllocationResult allocate(
        const std::vector<std::shared_ptr<strategy::IStrategy>>& strategies,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty) = 0;
};

/// Аллокатор на основе режима, мировой модели и неопределённости
class RegimeAwareAllocator : public IStrategyAllocator {
public:
    RegimeAwareAllocator(std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<clock::IClock> clock);

    AllocationResult allocate(
        const std::vector<std::shared_ptr<strategy::IStrategy>>& strategies,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const uncertainty::UncertaintySnapshot& uncertainty) override;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
};

} // namespace tb::strategy_allocator
