/**
 * @file pair_execution_coordinator.cpp
 * @brief Implementation of PairExecutionCoordinator.
 *
 * Coordinates dual-leg pair entry/exit through DualLegManager,
 * with orphan detection, fill skew monitoring, and cleanup.
 */

#include "pipeline/pair_execution_coordinator.hpp"
#include <cmath>
#include <format>

namespace tb::pipeline {

static constexpr char kComp[] = "PairExecCoord";

PairExecutionCoordinator::PairExecutionCoordinator(
    PairExecConfig config,
    std::shared_ptr<DualLegManager> dual_leg_mgr,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , dual_leg_mgr_(std::move(dual_leg_mgr))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

PairEntryExecResult PairExecutionCoordinator::execute_pair_entry(
    const PairDecision& decision,
    const Symbol& symbol,
    double base_size,
    double entry_price_hint)
{
    PairEntryExecResult result;

    if (!dual_leg_mgr_) {
        result.error = "DualLegManager not available";
        return result;
    }

    double hedge_size = base_size * decision.hedge_ratio;

    // Compute TPSL for each leg
    double long_tp = 0.0, long_sl = 0.0, short_tp = 0.0, short_sl = 0.0;
    if (config_.attach_server_tpsl) {
        dual_leg_mgr_->compute_tpsl(entry_price_hint, PositionSide::Long,
                                     long_tp, long_sl);
        dual_leg_mgr_->compute_tpsl(entry_price_hint, PositionSide::Short,
                                     short_tp, short_sl);
    }

    LegSpec long_spec;
    long_spec.side = PositionSide::Long;
    long_spec.size = base_size;
    long_spec.entry_price_hint = entry_price_hint;
    long_spec.tp_price = long_tp;
    long_spec.sl_price = long_sl;

    LegSpec short_spec;
    short_spec.side = PositionSide::Short;
    short_spec.size = hedge_size;
    short_spec.entry_price_hint = entry_price_hint;
    short_spec.tp_price = short_tp;
    short_spec.sl_price = short_sl;

    logger_->info(kComp, std::format("Pair entry: symbol={}, base={:.4f}, hedge={:.4f}, "
        "ratio={:.2f}, mode={}",
        symbol.get(), base_size, hedge_size, decision.hedge_ratio,
        to_string(decision.action)), {});

    int64_t entry_start_ns = clock_->now().get();
    auto pair_result = dual_leg_mgr_->enter_pair(symbol, long_spec, short_spec);

    if (!pair_result.success) {
        result.error = pair_result.error;
        // Check if an orphan was left
        {
            std::lock_guard lock(orphan_mutex_);
            if (dual_leg_mgr_->long_leg().state == LegState::Active ||
                dual_leg_mgr_->long_leg().state == LegState::PendingEntry) {
                orphan_detected_ = true;
                orphan_side_ = PositionSide::Long;
                orphan_detect_time_ns_ = clock_->now().get();
                logger_->warn(kComp, "Orphan long leg detected after entry failure", {});
            }
        }
        return result;
    }

    result.success = true;
    result.long_order_id = pair_result.long_leg.exchange_order_id.get();
    result.short_order_id = pair_result.short_leg.exchange_order_id.get();
    result.long_fill_qty = base_size;
    result.short_fill_qty = hedge_size;
    result.fill_skew_ns = clock_->now().get() - entry_start_ns;

    if (result.fill_skew_ns > static_cast<int64_t>(config_.max_fill_skew_ms * 1'000'000)) {
        logger_->warn(kComp, std::format("Fill skew {:.0f}ms exceeds limit {:.0f}ms",
            result.fill_skew_ns / 1e6, config_.max_fill_skew_ms), {});
    }

    active_symbol_ = symbol;
    {
        std::lock_guard lock(orphan_mutex_);
        orphan_detected_ = false;
    }
    return result;
}

PairCloseExecResult PairExecutionCoordinator::execute_pair_close(
    const PairState& state,
    double urgency)
{
    PairCloseExecResult result;

    if (!dual_leg_mgr_) {
        result.error = "DualLegManager not available";
        return result;
    }

    std::string reason = std::format("pair_close: urgency={:.2f}, net_pnl={:.1f}bps",
        urgency, state.net_pair_pnl_bps);

    active_symbol_ = state.symbol;

    bool ok = dual_leg_mgr_->close_both(state.symbol, reason);
    if (!ok) {
        result.error = "close_both failed";
        // Check for partial close (one leg may have succeeded)
        auto lleg = dual_leg_mgr_->long_leg();
        auto sleg = dual_leg_mgr_->short_leg();
        {
            std::lock_guard lock(orphan_mutex_);
            if (lleg.state == LegState::Active && sleg.state != LegState::Active) {
                orphan_detected_ = true;
                orphan_side_ = PositionSide::Long;
                orphan_detect_time_ns_ = clock_->now().get();
            } else if (sleg.state == LegState::Active && lleg.state != LegState::Active) {
                orphan_detected_ = true;
                orphan_side_ = PositionSide::Short;
                orphan_detect_time_ns_ = clock_->now().get();
            }
        }
        return result;
    }

    result.success = true;
    result.net_realized_pnl = state.net_pair_pnl_bps;
    {
        std::lock_guard lock(orphan_mutex_);
        orphan_detected_ = false;
    }
    return result;
}

bool PairExecutionCoordinator::close_single_leg(PositionSide side) {
    if (!dual_leg_mgr_) return false;
    if (!active_symbol_.has_value()) {
        logger_->error(kComp, "close_single_leg failed: active symbol is unknown", {});
        return false;
    }

    auto leg = (side == PositionSide::Long) ?
        dual_leg_mgr_->long_leg() : dual_leg_mgr_->short_leg();
    if (leg.state != LegState::Active || leg.size <= 0.0) {
        return false;
    }

    std::string reason = std::format("trim_leg: {}",
        side == PositionSide::Long ? "Long" : "Short");

    bool ok = dual_leg_mgr_->close_leg(*active_symbol_, side, leg.size, reason);
    if (!ok) {
        logger_->error(kComp, "close_single_leg failed",
            {{"side", side == PositionSide::Long ? "long" : "short"},
             {"symbol", active_symbol_->get()}});
        return false;
    }

    logger_->info(kComp, "Single leg close submitted",
        {{"side", side == PositionSide::Long ? "long" : "short"},
         {"symbol", active_symbol_->get()},
         {"size", std::to_string(leg.size)}});
    return true;
}

bool PairExecutionCoordinator::execute_reversal(PositionSide from_side,
    double size, const Symbol& symbol)
{
    if (!dual_leg_mgr_) return false;

    auto result = dual_leg_mgr_->reverse_leg(symbol, from_side, size);
    if (!result.success) {
        logger_->error(kComp, "Reversal failed: " + result.error, {});
        return false;
    }
    return true;
}

bool PairExecutionCoordinator::has_orphan_leg() const {
    std::lock_guard lock(orphan_mutex_);
    return orphan_detected_;
}

void PairExecutionCoordinator::cleanup_orphan_leg() {
    PositionSide side_to_close;
    {
        std::lock_guard lock(orphan_mutex_);
        if (!orphan_detected_ || !dual_leg_mgr_) return;
        side_to_close = orphan_side_;
    }

    auto leg = (side_to_close == PositionSide::Long) ?
        dual_leg_mgr_->long_leg() : dual_leg_mgr_->short_leg();

    if (!active_symbol_.has_value()) {
        logger_->error(kComp, "Orphan cleanup skipped: active symbol is unknown", {});
        return;
    }

    if (leg.state == LegState::Active && leg.size > 0.0) {
        logger_->warn(kComp, std::format("Cleaning up orphan {} leg, size={:.4f}",
            side_to_close == PositionSide::Long ? "Long" : "Short", leg.size),
            {{"symbol", active_symbol_->get()}});
        bool ok = dual_leg_mgr_->close_leg(*active_symbol_, side_to_close, leg.size,
            "orphan_cleanup");
        if (!ok) {
            logger_->error(kComp, "Orphan cleanup close_leg failed",
                {{"symbol", active_symbol_->get()},
                 {"side", side_to_close == PositionSide::Long ? "long" : "short"}});
            return;
        }
    }

    std::lock_guard lock(orphan_mutex_);
    orphan_detected_ = false;
}

} // namespace tb::pipeline
