#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::strategy_allocator {

/// Запись аллокации для одной стратегии
struct StrategyAllocation {
    StrategyId strategy_id{StrategyId("")};
    bool is_enabled{false};
    double weight{0.0};           ///< Вес стратегии [0.0, 1.0]
    double size_multiplier{1.0};  ///< Множитель размера позиции
    std::string reason;           ///< Причина текущего состояния аллокации
};

/// Полный результат аллокации стратегий
struct AllocationResult {
    std::vector<StrategyAllocation> allocations;
    std::size_t enabled_count{0};
    Timestamp computed_at{Timestamp(0)};
    std::string explanation;
};

} // namespace tb::strategy_allocator
