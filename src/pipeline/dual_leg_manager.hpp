#pragma once
/**
 * @file dual_leg_manager.hpp
 * @brief DualLegManager — координатор парного long+short входа (Phase E)
 *
 * Управляет жизненным циклом дуальных ног:
 * 1. Batch entry: координированный вход long + short
 * 2. Per-leg server-side TPSL via Bitget attached TP/SL
 * 3. Reversal via Bitget click-backhand API (atomic position flip)
 * 4. Funding-aware pair economics (auto-unwind if carry > edge)
 */

#include "common/types.hpp"
#include "pipeline/pair_economics.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace tb::execution { class ExecutionEngine; }
namespace tb::exchange::bitget {
class BitgetFuturesOrderSubmitter;
class BitgetRestClient;
}

namespace tb::pipeline {

/// State of a single leg in the pair
enum class LegState {
    Idle,              ///< No position
    PendingEntry,      ///< Order submitted, awaiting fill
    Active,            ///< Filled and holding
    PendingExit,       ///< Exit order submitted
    Closed             ///< Position closed
};

/// Configuration for the dual-leg manager
struct DualLegConfig {
    double tpsl_take_profit_pct{0.8};      ///< TP distance (% from entry) per leg
    double tpsl_stop_loss_pct{1.5};         ///< SL distance (% from entry) per leg
    double max_spread_bps_for_entry{15.0};  ///< Max spread to allow pair entry
    double min_edge_bps{5.0};               ///< Min expected edge to justify pair entry
    double max_funding_rate_abs{0.001};     ///< Max combined funding cost before auto-unwind
    int reversal_cooldown_ms{2000};         ///< Min time between reversals
    bool use_reversal_api{true};            ///< Use click-backhand for atomic reversal
    bool attach_server_tpsl{true};          ///< Attach TP/SL on order placement
};

/// Description of one leg for batch entry
struct LegSpec {
    PositionSide side{PositionSide::Long};
    double size{0.0};                        ///< Base currency quantity
    double entry_price_hint{0.0};            ///< Expected fill price
    double tp_price{0.0};                    ///< Server-side take-profit price (0=none)
    double sl_price{0.0};                    ///< Server-side stop-loss price (0=none)
};

/// Live state of one leg
struct LegSnapshot {
    LegState state{LegState::Idle};
    PositionSide side{PositionSide::Long};
    OrderId order_id{OrderId("")};
    OrderId exchange_order_id{OrderId("")};
    double size{0.0};
    double entry_price{0.0};
    double unrealized_pnl{0.0};
    double unrealized_pnl_pct{0.0};
    int64_t entry_time_ns{0};
    bool has_server_tp{false};
    bool has_server_sl{false};
};

/// Result of a pair entry attempt
struct PairEntryResult {
    bool success{false};
    LegSnapshot long_leg;
    LegSnapshot short_leg;
    std::string error;
};

/// Result of a reversal (one leg flips to opposite)
struct ReversalResult {
    bool success{false};
    OrderId new_order_id{OrderId("")};
    std::string error;
};

/// DualLegManager — orchestrates coordinated long+short pair operations
class DualLegManager {
public:
    DualLegManager(DualLegConfig config,
                   std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter,
                   std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                   std::shared_ptr<logging::ILogger> logger,
                   std::shared_ptr<clock::IClock> clock);

    /// Batch entry: submit long+short as a coordinated pair.
    /// Both legs are submitted sequentially (long first, then short).
    /// If first leg fails, no second leg is attempted.
    /// If second leg fails, first leg is cancelled.
    PairEntryResult enter_pair(const Symbol& symbol,
                               const LegSpec& long_spec,
                               const LegSpec& short_spec);

    /// Atomic reversal of a leg via click-backhand API.
    /// Flips position side (long→short or short→long) in one exchange operation.
    /// Falls back to close+reopen if reversal API fails.
    ReversalResult reverse_leg(const Symbol& symbol,
                               PositionSide current_side,
                               double size);

    /// Close one specific leg
    bool close_leg(const Symbol& symbol, PositionSide side, double size,
                   const std::string& reason);

    /// Close both legs (emergency or profit-lock)
    bool close_both(const Symbol& symbol, const std::string& reason);

    /// Check if pair carry cost exceeds acceptable threshold.
    /// Returns true if combined funding cost > config.max_funding_rate_abs
    bool is_carry_too_expensive(double funding_rate) const;

    /// Compute per-leg TPSL prices given entry price and config
    void compute_tpsl(double entry_price, PositionSide side,
                      double& tp_price, double& sl_price) const;

    /// Get current leg states (thread-safe copies)
    LegSnapshot long_leg()  const { std::lock_guard lock(state_mutex_); return long_leg_; }
    LegSnapshot short_leg() const { std::lock_guard lock(state_mutex_); return short_leg_; }
    bool has_active_pair() const;

    /// Reset state (after both legs closed)
    void reset();

    /// Update leg with fill information
    void update_leg(PositionSide side, double fill_price, double fill_qty,
                    int64_t fill_time_ns);

    /// Get config (for pipeline to read thresholds)
    const DualLegConfig& config() const { return config_; }

private:
    DualLegConfig config_;
    std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter_;
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    // BUG-S5-05 fix: LegSnapshot state accessed concurrently from fill callbacks
    // (update_leg) and the pipeline thread (close_leg, has_active_pair, getters).
    // All leg state mutations and reads must hold state_mutex_.
    mutable std::mutex state_mutex_;
    LegSnapshot long_leg_;
    LegSnapshot short_leg_;
    int64_t last_reversal_ns_{0};
};

} // namespace tb::pipeline
