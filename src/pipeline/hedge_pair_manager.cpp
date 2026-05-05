#include "pipeline/hedge_pair_manager.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace tb::pipeline {

HedgePairManager::HedgePairManager(std::shared_ptr<logging::ILogger> logger)
    : logger_(std::move(logger)) {}

void HedgePairManager::reset() {
    state_ = HedgePairState::PrimaryOnly;
    hedge_count_ = 0;
}

bool HedgePairManager::can_hedge() const {
    return state_ == HedgePairState::PrimaryOnly
        && hedge_count_ < kMaxHedgesPerPosition;
}

void HedgePairManager::notify_hedge_opened() {
    // Transition to pending — portfolio confirmation (has_hedge=true) will
    // increment hedge_count_ and advance to PrimaryPlusHedge.  This prevents
    // counting unfilled orders against the per-position hedge budget.
    state_ = HedgePairState::HedgeSentPending;
}

void HedgePairManager::notify_hedge_failed() {
    // Order was cancelled or rejected without ever filling.
    state_ = HedgePairState::PrimaryOnly;
}

void HedgePairManager::notify_hedge_closed() {
    state_ = HedgePairState::PrimaryOnly;
}

void HedgePairManager::notify_both_closed() {
    state_ = HedgePairState::PrimaryOnly;
}

void HedgePairManager::notify_reversed() {
    state_ = HedgePairState::PrimaryOnly;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN EVALUATION
// ═══════════════════════════════════════════════════════════════════════════════

HedgePairDecision HedgePairManager::evaluate(const HedgePairInput& input) {
    switch (state_) {
    case HedgePairState::PrimaryOnly:
        return evaluate_primary_only(input);

    case HedgePairState::HedgeSentPending:
        return evaluate_hedge_pending(input);

    case HedgePairState::PrimaryPlusHedge:
    case HedgePairState::ProfitLockPair:
    case HedgePairState::AsymmetricUnwind:
        return evaluate_pair_active(input);

    case HedgePairState::ReverseTransition:
        // Reverse was decided — pipeline is executing the close
        return {};

    case HedgePairState::EmergencyFlatten:
        // Emergency flatten requested — close both
        return {HedgeAction::CloseBoth, 0.0, "Emergency flatten active", 1.0};

    default:
        // BUG-S24-09: new enum value added without updating switch → silent no-op
        if (logger_) logger_->error("HedgePairManager",
            "evaluate(): unhandled HedgePairState — returning NoAction",
            {{"state", std::to_string(static_cast<int>(state_))}});
        return {};
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PRIMARY ONLY — hedge trigger evaluation
// Trigger is market-driven: regime change + indicator confirmation
// NOT just loss-percentage based
// ═══════════════════════════════════════════════════════════════════════════════

bool HedgePairManager::should_trigger_hedge(const HedgePairInput& input) const {
    if (!can_hedge()) return false;

    // Position must be in loss to justify a hedge
    if (input.primary_pnl >= 0.0) return false;
    double loss_pct = std::abs(input.primary_pnl) / std::max(input.total_capital, 0.01) * 100.0;

    bool primary_long = (input.primary_side == PositionSide::Long);
    bool momentum_against = input.momentum_valid &&
        (primary_long ? (input.momentum < -0.0005) : (input.momentum > 0.0005));

    // Market-driven trigger: require AT LEAST ONE of these confirmed scenarios:
    //
    // Scenario A: Regime change confirmed
    //   CUSUM detected structural break + regime stability low
    bool regime_break = input.cusum_regime_change && input.regime_stability < 0.35;

    // Scenario B: Indicator consensus strongly against position
    //   exit_score < -0.15 means 3+ indicators confirm adverse trend
    bool indicator_consensus = input.exit_score_primary < -0.15;

    // Scenario B2: protective stop is near and the move keeps accelerating against us.
    // This catches fast adverse moves that hit ATR/protective stops before
    // capital-loss thresholds would justify a plain close.
    bool protective_stop_pressure = input.protective_stop_imminent &&
        (input.exit_score_primary < -0.05 || momentum_against);

    bool loss_budget_breached = loss_pct >= input.hedge_trigger_loss_pct;
    if (!loss_budget_breached && !protective_stop_pressure) return false;

    // Scenario C: Toxic flow detected while in loss
    //   VPIN spike + position losing: smart money is moving against us
    bool toxic_while_losing = input.vpin_toxic && loss_pct > 0.8;

    // Scenario D: Liquidity stress
    //   Thin book + wide spread + in loss: exit would be costly, hedge is better
    bool liquidity_stress = input.depth_usd < 500.0 && input.spread_bps > 30.0
                         && loss_pct > 1.0;

    // Require at least one confirmed market scenario
    if (!regime_break && !indicator_consensus && !protective_stop_pressure
        && !toxic_while_losing && !liquidity_stress) {
        return false;
    }

    // Additional filter: don't hedge in very high uncertainty (market is chaotic)
    if (input.uncertainty > 0.85) return false;

    return true;
}

double HedgePairManager::compute_hedge_ratio(const HedgePairInput& input) const {
    // Base ratio: not always 1:1
    // Determined by volatility, liquidity, and signal strength
    double ratio = 0.7;  // Default: partial hedge (70% of primary)

    if (input.protective_stop_imminent) {
        ratio = std::max(ratio, 1.0);
    }

    // Strong regime break → full hedge
    if (input.cusum_regime_change && input.regime_stability < 0.25) {
        ratio = 1.0;
    }

    // Very strong indicator consensus → larger hedge
    if (input.exit_score_primary < -0.30) {
        ratio = std::min(ratio + 0.2, 1.2);
    }

    // Toxic flow → full hedge
    if (input.vpin_toxic) {
        ratio = std::max(ratio, 1.0);
    }

    // High funding cost for primary side → smaller hedge (hedging is also paying)
    // BUG-S8-09: NaN funding_rate makes eff_funding NaN; the comparison silently
    // returns false, skipping the penalty. Guard before use.
    if (std::isfinite(input.funding_rate)) {
        bool primary_long = (input.primary_side == PositionSide::Long);
        double eff_funding = primary_long ? input.funding_rate : -input.funding_rate;
        if (eff_funding > 0.0005) {
            ratio *= 0.8;  // Both sides pay in opposite directions, reduce exposure
        }
    }

    // Thin liquidity → smaller hedge to avoid moving market
    if (input.depth_usd < 1000.0) {
        ratio = std::min(ratio, 0.8);
    }

    return std::clamp(ratio, 0.3, 1.2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HEDGE SENT PENDING — waiting for portfolio to confirm fill
// hedge_count_ is NOT yet incremented; only advance once portfolio shows the leg.
// ═══════════════════════════════════════════════════════════════════════════════

HedgePairDecision HedgePairManager::evaluate_hedge_pending(const HedgePairInput& input) {
    if (input.has_hedge) {
        // Portfolio confirmed the hedge leg — commit the count and enter active mode.
        ++hedge_count_;
        state_ = HedgePairState::PrimaryPlusHedge;
        logger_->info("pipeline", "HEDGE confirmed by portfolio — entering pair mode",
            {{"hedge_count", std::to_string(hedge_count_)}});
        return evaluate_pair_active(input);
    }
    // Portfolio hasn't reflected the fill yet (or order is still in-flight).
    // Stay in pending — pipeline's order watchdog will call notify_hedge_failed()
    // if the order is cancelled.
    return {};
}

HedgePairDecision HedgePairManager::evaluate_primary_only(const HedgePairInput& input) {
    if (!should_trigger_hedge(input)) return {};

    double ratio = compute_hedge_ratio(input);
    double loss_pct = std::abs(input.primary_pnl) / std::max(input.total_capital, 0.01) * 100.0;

    std::string reason = std::format(
        "Hedge trigger: loss={:.2f}%, score={:.2f}, regime_stab={:.2f}, "
        "cusum={}, vpin={}, stop_imminent={}, stop_gap={:.3f}%, ratio={:.2f}",
        loss_pct, input.exit_score_primary, input.regime_stability,
        input.cusum_regime_change, input.vpin_toxic,
        input.protective_stop_imminent, input.stop_distance_pct, ratio);

    logger_->warn("pipeline", "HEDGE PAIR: trigger confirmed",
        {{"reason", reason},
         {"state", to_string(state_)},
         {"hedge_count", std::to_string(hedge_count_)}});

    return {HedgeAction::OpenHedge, ratio, std::move(reason), 0.9};
}

// ═══════════════════════════════════════════════════════════════════════════════
// PAIR ACTIVE — manage the two-leg position
// Decisions based on paired economics, not timeout
// ═══════════════════════════════════════════════════════════════════════════════

HedgePairDecision HedgePairManager::evaluate_pair_active(const HedgePairInput& input) {
    if (!input.has_hedge) {
        // Hedge leg disappeared (filled, reconciled) — reset to primary only
        state_ = HedgePairState::PrimaryOnly;
        return {};
    }

    double net_pnl = input.primary_pnl + input.hedge_pnl;
    double total_notional = (input.primary_size + input.hedge_size) * input.mid_price;
    double round_trip_fees = total_notional * input.taker_fee_pct / 100.0 * 2.0;
    double min_profit = round_trip_fees * input.hedge_profit_close_fee_mult;

    // ─── Emergency: stale data or extreme conditions ──────────────────
    if (input.vpin_toxic && net_pnl < -round_trip_fees * 3.0) {
        state_ = HedgePairState::EmergencyFlatten;
        return {HedgeAction::CloseBoth, 0.0,
            std::format("Emergency: toxic flow + deep pair loss {:.2f}", net_pnl), 1.0};
    }

    // ─── Strategy 1: Net profit covers fees → close both ─────────────
    if (net_pnl > min_profit) {
        state_ = HedgePairState::ProfitLockPair;
        return {HedgeAction::CloseBoth, 0.0,
            std::format("Pair net profit {:.2f} > {:.2f} fee threshold", net_pnl, min_profit),
            0.8};
    }

    // ─── Strategy 2: Asymmetric unwind based on market state ─────────
    // Close the leg that has captured its value when market turns for the other.
    // Key difference from old code: no timeout, purely market-driven.

    bool is_hedge_long = (input.primary_side == PositionSide::Long)
        ? false : true;  // Hedge is opposite of primary

    // 2a: Hedge leg profitable + market turning for primary → close hedge
    if (input.hedge_pnl > round_trip_fees) {
        bool momentum_for_primary = false;
        if (input.momentum_valid) {
            bool primary_long = (input.primary_side == PositionSide::Long);
            momentum_for_primary = primary_long
                ? (input.momentum > 0.0005)
                : (input.momentum < -0.0005);
        }
        bool primary_recovering = input.exit_score_primary > 0.10;
        bool regime_stabilizing = input.regime_stability > 0.5 && !input.cusum_regime_change;
        double unwind_confidence = 0.0;
        if (momentum_for_primary) unwind_confidence += 0.45;
        if (primary_recovering) unwind_confidence += 0.35;
        if (regime_stabilizing) unwind_confidence += 0.20;
        if (regime_stabilizing && input.hedge_pnl > round_trip_fees * 2.0) {
            // Strongly profitable hedge in a stabilized regime can be unwound
            // even without momentum confirmation.
            unwind_confidence += 0.35;
        }

        if (unwind_confidence >= 0.55) {
            state_ = HedgePairState::AsymmetricUnwind;
            return {HedgeAction::CloseHedge, 0.0,
                std::format("Unwind hedge: hedge_pnl={:.2f}, primary_score={:.2f}, "
                    "regime_stab={:.2f}, momentum_ok={}, confidence={:.2f}",
                    input.hedge_pnl, input.exit_score_primary,
                    input.regime_stability, momentum_for_primary, unwind_confidence),
                std::clamp(0.55 + unwind_confidence * 0.30, 0.0, 1.0)};
        }
    }

    // 2b: Primary leg profitable + market turning for hedge → close primary (reverse)
    if (input.primary_pnl > round_trip_fees) {
        bool momentum_for_hedge = false;
        if (input.momentum_valid) {
            momentum_for_hedge = is_hedge_long
                ? (input.momentum > 0.0005)
                : (input.momentum < -0.0005);
        }
        bool hedge_strengthening = input.exit_score_hedge > 0.10;
        bool regime_break_continues = input.cusum_regime_change && input.regime_stability < 0.40;
        double reverse_confidence = 0.0;
        if (momentum_for_hedge) reverse_confidence += 0.45;
        if (hedge_strengthening) reverse_confidence += 0.35;
        if (regime_break_continues) reverse_confidence += 0.20;

        if (reverse_confidence >= 0.55) {
            state_ = HedgePairState::ReverseTransition;
            return {HedgeAction::ClosePrimary, 0.0,
                std::format("Reverse: primary_pnl={:.2f}, hedge_score={:.2f}, "
                    "momentum_for_hedge={}, confidence={:.2f}",
                    input.primary_pnl, input.exit_score_hedge,
                    momentum_for_hedge, reverse_confidence),
                std::clamp(0.55 + reverse_confidence * 0.30, 0.0, 1.0)};
        }
    }

    // ─── Strategy 3: Market-driven escalation (replaces blind timeout) ──
    // Market-driven escalation: urgency based on adverse market conditions,
    // not hold duration.

    // Escalation conditions (any triggers escalation):
    bool funding_expensive = false;
    {
        bool primary_long = (input.primary_side == PositionSide::Long);
        double eff_funding = primary_long ? input.funding_rate : -input.funding_rate;
        // Both legs are paying in high-funding environment
        funding_expensive = std::abs(input.funding_rate) > 0.0008;
    }

    bool pair_degrading = net_pnl < -round_trip_fees * 4.0;
    bool both_scores_poor = input.exit_score_primary < -0.05 && input.exit_score_hedge < -0.05;
    bool spread_widening = input.spread_bps > 25.0;
    bool toxic_flow = input.vpin_toxic;

    // Market-driven escalation only — no time-based triggers.
    // Require at least 2 adverse market conditions to escalate.
    int escalation_count = (funding_expensive ? 1 : 0) + (pair_degrading ? 1 : 0)
                         + (both_scores_poor ? 1 : 0) + (spread_widening ? 1 : 0)
                         + (toxic_flow ? 1 : 0);

    if (escalation_count >= 2) {
        // Close both if net is positive-ish, otherwise close the better leg
        if (net_pnl > -round_trip_fees) {
            state_ = HedgePairState::ProfitLockPair;
            return {HedgeAction::CloseBoth, 0.0,
                std::format("Escalation close both: net_pnl={:.2f}, factors={}",
                    net_pnl, escalation_count),
                0.8};
        }
        // Close the leg with better PnL (preserve value)
        if (input.hedge_pnl > input.primary_pnl) {
            state_ = HedgePairState::AsymmetricUnwind;
            return {HedgeAction::CloseHedge, 0.0,
                std::format("Escalation unwind hedge (better): hedge_pnl={:.2f}, "
                    "primary_pnl={:.2f}, factors={}",
                    input.hedge_pnl, input.primary_pnl, escalation_count),
                0.75};
        } else {
            state_ = HedgePairState::ReverseTransition;
            return {HedgeAction::ClosePrimary, 0.0,
                std::format("Escalation reverse (primary better): primary_pnl={:.2f}, "
                    "hedge_pnl={:.2f}, factors={}",
                    input.primary_pnl, input.hedge_pnl, escalation_count),
                0.75};
        }
    }

    // No action needed — pair is stable
    return {};
}

} // namespace tb::pipeline
