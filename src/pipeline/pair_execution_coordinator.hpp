#pragma once
/**
 * @file pair_execution_coordinator.hpp
 * @brief Coordinates pair entry/exit execution with batch orders and server-side protection.
 *
 * Handles:
 * - Batch dual-leg entry via Bitget batch-place-order API
 * - Coordinated pair close (both legs simultaneously)
 * - Server-side TPSL attachment per leg
 * - Orphan leg detection and cleanup
 * - Fill skew monitoring between legs
 */

#include "common/types.hpp"
#include "pipeline/pair_lifecycle_engine.hpp"
#include "pipeline/dual_leg_manager.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace tb::pipeline {

// ============================================================
// Execution result types
// ============================================================

struct PairEntryExecResult {
    bool success{false};
    std::string long_order_id;
    std::string short_order_id;
    double long_fill_price{0.0};
    double short_fill_price{0.0};
    double long_fill_qty{0.0};
    double short_fill_qty{0.0};
    int64_t fill_skew_ns{0};            ///< Time difference between leg fills
    std::string error;
};

struct PairCloseExecResult {
    bool success{false};
    double net_realized_pnl{0.0};
    std::string error;
};

// ============================================================
// Coordinator config
// ============================================================

struct PairExecConfig {
    double max_fill_skew_ms{150.0};     ///< Max acceptable fill time skew
    bool enable_batch_entry{true};       ///< Use batch-place-order for dual entry
    bool enable_batch_close{true};       ///< Use batch for closing both legs
    bool attach_server_tpsl{true};       ///< Attach server-side TP/SL to each leg
    std::string stp_mode{"cancel_taker"};///< Self-trade prevention mode
};

// ============================================================
// PairExecutionCoordinator
// ============================================================

class PairExecutionCoordinator {
public:
    PairExecutionCoordinator(
        PairExecConfig config,
        std::shared_ptr<DualLegManager> dual_leg_mgr,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock);

    /// Execute a pair entry based on PairDecision
    [[nodiscard]] PairEntryExecResult execute_pair_entry(
        const PairDecision& decision,
        const Symbol& symbol,
        double base_size,
        double entry_price_hint);

    /// Execute coordinated pair close
    [[nodiscard]] PairCloseExecResult execute_pair_close(
        const PairState& state,
        double urgency);

    /// Close a single leg (for trim/promote operations)
    [[nodiscard]] bool close_single_leg(PositionSide side);

    /// Execute atomic reversal via click-backhand
    [[nodiscard]] bool execute_reversal(PositionSide from_side, double size,
        const Symbol& symbol);

    /// Check for orphan legs (one leg filled, other not)
    [[nodiscard]] bool has_orphan_leg() const;

    /// Cleanup orphan leg if detected
    void cleanup_orphan_leg();

private:
    PairExecConfig config_;
    std::shared_ptr<DualLegManager> dual_leg_mgr_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    // BUG-S8-04: orphan fields written from execute_pair_entry/close,
    // read from has_orphan_leg/cleanup_orphan_leg — protect with mutex.
    mutable std::mutex orphan_mutex_;
    bool orphan_detected_{false};
    PositionSide orphan_side_{PositionSide::Long};
    int64_t orphan_detect_time_ns_{0};

    // Symbol context for single-leg and orphan cleanup operations.
    std::optional<Symbol> active_symbol_;
};

} // namespace tb::pipeline
