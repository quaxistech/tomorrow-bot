#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::decision {

/// Allocation record for a single strategy.
/// Kept as a thin pass-through type for the scalping bot: there is exactly one
/// active strategy (MomentumContinuation per EDGE-15/EDGE-29). The committee
/// allocator was removed; pipeline now produces this struct inline from
/// regime/world/uncertainty multipliers without a dedicated engine.
struct StrategyAllocation {
    StrategyId strategy_id{StrategyId("")};
    bool is_enabled{false};
    double weight{0.0};           ///< Effective weight in [0, 1].
    double size_multiplier{1.0};  ///< Position-size multiplier.
    std::string reason;
};

struct AllocationResult {
    std::vector<StrategyAllocation> allocations;
    std::size_t enabled_count{0};
    Timestamp computed_at{Timestamp(0)};
    std::string explanation;
};

} // namespace tb::decision
