#pragma once
/**
 * @file pair_lifecycle_engine.hpp
 * @brief Unified pair-native lifecycle engine for coordinated long/short trading.
 *
 * Subsumes logic from DualLegManager (physical execution) and HedgePairManager
 * (decision-making), providing four production pair modes:
 *   1. CompressionPair  — balanced L+S during range/compression regimes
 *   2. TransitionPair   — asymmetric pair during uncertain regime transitions
 *   3. BreakoutShield   — directional + small protective opposite leg
 *   4. AsymmetricRunner — strong leg only after directional confirmation
 *
 * All pair decisions are market-driven. No time-based exit triggers.
 */

#include "common/types.hpp"
#include "pipeline/dual_leg_manager.hpp"
#include "pipeline/hedge_pair_manager.hpp"
#include "pipeline/pair_economics.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <memory>
#include <optional>
#include <string>

namespace tb::pipeline {

// ============================================================
// Pair modes
// ============================================================

enum class PairMode {
    NoPair,              ///< No pair active — single-leg or idle
    CompressionPair,     ///< Balanced L+S in range/compression regime
    TransitionPair,      ///< Asymmetric pair during regime transition
    BreakoutShield,      ///< Directional + small protective leg
    AsymmetricRunner     ///< Strong single leg after pair unwound
};

[[nodiscard]] inline const char* to_string(PairMode m) noexcept {
    switch (m) {
        case PairMode::NoPair:           return "NoPair";
        case PairMode::CompressionPair:  return "CompressionPair";
        case PairMode::TransitionPair:   return "TransitionPair";
        case PairMode::BreakoutShield:   return "BreakoutShield";
        case PairMode::AsymmetricRunner: return "AsymmetricRunner";
        default:                         return "Unknown";
    }
}

// ============================================================
// Pair leg snapshot
// ============================================================

struct PairLeg {
    PositionSide side{PositionSide::Long};
    double size{0.0};
    double entry_price{0.0};
    double unrealized_pnl_bps{0.0};
    double continuation_score{0.0};     ///< Market-driven hold quality
    double protective_stop{0.0};        ///< Server-side stop level
    bool active{false};
};

// ============================================================
// Full pair state snapshot
// ============================================================

struct PairState {
    Symbol symbol{""};
    PairMode mode{PairMode::NoPair};
    PairLeg long_leg;
    PairLeg short_leg;

    double net_pair_pnl_bps{0.0};       ///< Net PnL of pair in basis points
    double gross_notional{0.0};          ///< Total notional across both legs
    double net_exposure{0.0};            ///< Long notional - Short notional
    double funding_drag_bps{0.0};        ///< Accumulated funding cost in bps
    double pair_quality{0.0};            ///< Composite quality score [0,1]
};

// ============================================================
// Pair lifecycle action
// ============================================================

enum class PairAction {
    None,                   ///< Hold — no change to pair
    OpenCompressionPair,    ///< Open balanced L+S pair
    OpenTransitionPair,     ///< Open asymmetric pair
    OpenBreakoutShield,     ///< Open directional + protective leg
    TrimWeakLeg,            ///< Reduce or close the weaker leg
    PromoteToRunner,        ///< Remove protective leg, run strong leg
    RebalancePair,          ///< Adjust hedge ratio
    CloseBothProfit,        ///< Close both legs with net profit
    CloseBothEmergency,     ///< Close both legs on emergency signal
    ReverseLeg              ///< Flip a leg via click-backhand
};

[[nodiscard]] inline const char* to_string(PairAction a) noexcept {
    switch (a) {
        case PairAction::None:                 return "None";
        case PairAction::OpenCompressionPair:  return "OpenCompressionPair";
        case PairAction::OpenTransitionPair:   return "OpenTransitionPair";
        case PairAction::OpenBreakoutShield:   return "OpenBreakoutShield";
        case PairAction::TrimWeakLeg:          return "TrimWeakLeg";
        case PairAction::PromoteToRunner:      return "PromoteToRunner";
        case PairAction::RebalancePair:        return "RebalancePair";
        case PairAction::CloseBothProfit:      return "CloseBothProfit";
        case PairAction::CloseBothEmergency:   return "CloseBothEmergency";
        case PairAction::ReverseLeg:           return "ReverseLeg";
        default:                               return "Unknown";
    }
}

// ============================================================
// Pair decision output
// ============================================================

struct PairDecision {
    PairAction action{PairAction::None};
    double hedge_ratio{1.0};            ///< Target hedge ratio [0.2, 1.2]
    double urgency{0.0};                ///< [0, 1]
    std::string reason;
    PairState state;
};

// ============================================================
// Market input for pair evaluation
// ============================================================

struct PairMarketInput {
    Symbol symbol{""};

    // Regime
    double regime_stability{0.5};
    double regime_confidence{0.5};
    bool cusum_regime_change{false};

    // Uncertainty & world model
    double uncertainty{0.5};
    double p_continue{0.5};
    double p_reversal{0.25};
    double p_shock{0.05};

    // Microstructure
    double spread_bps{0.0};
    double depth_usd{0.0};
    bool vpin_toxic{false};
    double toxic_flow_score{0.0};

    // Market dynamics
    double trend_persistence{0.0};
    double momentum{0.0};
    bool momentum_valid{false};

    // Funding & fees
    double funding_rate{0.0};
    double taker_fee_pct{0.06};

    // Position state
    double atr{0.0};
    double mid_price{0.0};
    double total_capital{0.0};

    // Exit scores (from exit orchestrator)
    double exit_score_primary{0.0};
    double exit_score_hedge{0.0};

    // Primary position
    PositionSide primary_side{PositionSide::Long};
    double primary_size{0.0};
    double primary_pnl{0.0};
    double primary_pnl_pct{0.0};
    int64_t primary_hold_ns{0};

    // Hedge position
    bool has_hedge{false};
    double hedge_size{0.0};
    double hedge_pnl{0.0};
    double hedge_pnl_pct{0.0};
    int64_t hedge_hold_ns{0};
};

// ============================================================
// PairLifecycleEngine config
// ============================================================

struct PairLifecycleConfig {
    // Compression pair thresholds
    double compression_min_regime_stability{0.55};
    double compression_max_spread_bps{30.0};
    double compression_min_depth_usd{500.0};

    // Transition pair thresholds
    double transition_min_p_reversal{0.15};
    double transition_max_uncertainty{0.80};

    // Breakout shield thresholds
    double breakout_min_trend_persistence{0.30};
    double breakout_max_shock_prob{0.25};

    // Profit taking
    double pair_profit_floor_bps{6.0};
    double min_pair_quality{0.55};

    // Risk
    double max_unhedged_leg_ms{250.0};
    double emergency_loss_threshold_bps{-50.0};

    // Hedge trigger (from HedgePairManager)
    double hedge_trigger_loss_pct{1.5};
};

// ============================================================
// PairLifecycleEngine
// ============================================================

class PairLifecycleEngine {
public:
    PairLifecycleEngine(
        PairLifecycleConfig config,
        std::shared_ptr<DualLegManager> dual_leg_mgr,
        std::shared_ptr<HedgePairManager> hedge_pair_mgr,
        std::shared_ptr<PairEconomicsTracker> economics,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    /// Evaluate current market conditions and pair state, return decision
    [[nodiscard]] PairDecision evaluate(const PairMarketInput& input);

    /// Current pair mode
    [[nodiscard]] PairMode mode() const noexcept { return mode_; }

    /// Current pair state snapshot
    [[nodiscard]] PairState state() const noexcept { return state_; }

    /// Notify that a pair was successfully opened
    void notify_pair_opened(PairMode mode, const PairLeg& long_leg, const PairLeg& short_leg);

    /// Notify that both legs were closed
    void notify_pair_closed(double net_pnl_bps);

    /// Notify that weak leg was trimmed (transition to AsymmetricRunner)
    void notify_leg_trimmed(PositionSide trimmed_side);

    /// Reset state to NoPair
    void reset();

private:
    PairLifecycleConfig config_;
    std::shared_ptr<DualLegManager> dual_leg_mgr_;
    std::shared_ptr<HedgePairManager> hedge_pair_mgr_;
    std::shared_ptr<PairEconomicsTracker> economics_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    PairMode mode_{PairMode::NoPair};
    PairState state_;

    // Decision methods by state
    [[nodiscard]] PairDecision evaluate_no_pair(const PairMarketInput& input);
    [[nodiscard]] PairDecision evaluate_compression(const PairMarketInput& input);
    [[nodiscard]] PairDecision evaluate_transition(const PairMarketInput& input);
    [[nodiscard]] PairDecision evaluate_breakout_shield(const PairMarketInput& input);
    [[nodiscard]] PairDecision evaluate_asymmetric_runner(const PairMarketInput& input);

    // Helpers
    [[nodiscard]] PairMode select_pair_mode(const PairMarketInput& input) const;
    [[nodiscard]] double compute_hedge_ratio(const PairMarketInput& input) const;
    [[nodiscard]] double compute_pair_quality(const PairMarketInput& input) const;
    [[nodiscard]] double compute_pair_close_score(const PairMarketInput& input) const;
    void update_state(const PairMarketInput& input);
};

} // namespace tb::pipeline
