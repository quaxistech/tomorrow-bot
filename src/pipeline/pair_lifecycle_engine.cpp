/**
 * @file pair_lifecycle_engine.cpp
 * @brief Unified pair-native lifecycle engine implementation.
 *
 * Coordinates DualLegManager (execution) and HedgePairManager (legacy decisions)
 * into a single pair state machine with four market-driven modes.
 */

#include "pipeline/pair_lifecycle_engine.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace tb::pipeline {

// ============================================================
// Construction
// ============================================================

PairLifecycleEngine::PairLifecycleEngine(
    PairLifecycleConfig config,
    std::shared_ptr<DualLegManager> dual_leg_mgr,
    std::shared_ptr<HedgePairManager> hedge_pair_mgr,
    std::shared_ptr<PairEconomicsTracker> economics,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , dual_leg_mgr_(std::move(dual_leg_mgr))
    , hedge_pair_mgr_(std::move(hedge_pair_mgr))
    , economics_(std::move(economics))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{}

// ============================================================
// Main evaluation entry point
// ============================================================

PairDecision PairLifecycleEngine::evaluate(const PairMarketInput& input) {
    update_state(input);

    switch (mode_) {
        case PairMode::NoPair:
            return evaluate_no_pair(input);
        case PairMode::CompressionPair:
            return evaluate_compression(input);
        case PairMode::TransitionPair:
            return evaluate_transition(input);
        case PairMode::BreakoutShield:
            return evaluate_breakout_shield(input);
        case PairMode::AsymmetricRunner:
            return evaluate_asymmetric_runner(input);
        default:
            return PairDecision{.action = PairAction::None, .state = state_};
    }
}

// ============================================================
// NoPair: decide whether to open a pair
// ============================================================

PairDecision PairLifecycleEngine::evaluate_no_pair(const PairMarketInput& input) {
    // If no primary position exists, nothing to do
    if (input.primary_size <= 0.0) {
        return PairDecision{.action = PairAction::None, .state = state_};
    }

    // Select best pair mode based on market conditions
    PairMode target_mode = select_pair_mode(input);

    switch (target_mode) {
        case PairMode::CompressionPair: {
            double ratio = compute_hedge_ratio(input);
            return PairDecision{
                .action = PairAction::OpenCompressionPair,
                .hedge_ratio = std::clamp(ratio, 0.85, 1.15),
                .urgency = 0.4,
                .reason = std::format("Compression regime: stability={:.2f}, spread={:.1f}bps",
                    input.regime_stability, input.spread_bps),
                .state = state_
            };
        }
        case PairMode::TransitionPair: {
            double ratio = compute_hedge_ratio(input);
            return PairDecision{
                .action = PairAction::OpenTransitionPair,
                .hedge_ratio = std::clamp(ratio, 0.35, 0.60),
                .urgency = 0.5,
                .reason = std::format("Regime transition: p_reversal={:.2f}, uncertainty={:.2f}",
                    input.p_reversal, input.uncertainty),
                .state = state_
            };
        }
        case PairMode::BreakoutShield: {
            return PairDecision{
                .action = PairAction::OpenBreakoutShield,
                .hedge_ratio = std::clamp(0.20 + input.p_shock * 0.5, 0.20, 0.35),
                .urgency = 0.6,
                .reason = std::format("Breakout with protection: trend={:.2f}, p_shock={:.2f}",
                    input.trend_persistence, input.p_shock),
                .state = state_
            };
        }
        default:
            return PairDecision{.action = PairAction::None, .state = state_};
    }
}

// ============================================================
// CompressionPair: balanced L+S
// ============================================================

PairDecision PairLifecycleEngine::evaluate_compression(const PairMarketInput& input) {
    double close_score = compute_pair_close_score(input);
    double quality = compute_pair_quality(input);

    // Emergency: toxic flow + deep loss
    if (input.vpin_toxic && state_.net_pair_pnl_bps < config_.emergency_loss_threshold_bps) {
        return PairDecision{
            .action = PairAction::CloseBothEmergency,
            .urgency = 1.0,
            .reason = std::format("Emergency: toxic_flow + net_pnl={:.1f}bps", state_.net_pair_pnl_bps),
            .state = state_
        };
    }

    // Profit take: net PnL exceeds floor and regime returning to compression
    if (close_score > config_.pair_profit_floor_bps && quality < config_.min_pair_quality) {
        return PairDecision{
            .action = PairAction::CloseBothProfit,
            .urgency = std::clamp(close_score / 20.0, 0.4, 0.9),
            .reason = std::format("Profit lock: close_score={:.1f}bps, quality={:.2f}",
                close_score, quality),
            .state = state_
        };
    }

    // Regime has transitioned to directional — promote to runner
    if (std::abs(input.trend_persistence) > 0.45 && input.regime_stability > 0.60) {
        PositionSide strong_side = (input.trend_persistence > 0) ?
            PositionSide::Long : PositionSide::Short;
        PositionSide weak_side = (strong_side == PositionSide::Long) ?
            PositionSide::Short : PositionSide::Long;

        return PairDecision{
            .action = PairAction::TrimWeakLeg,
            .urgency = 0.6,
            .reason = std::format("Compression→Runner: trend={:.2f}, trim {}",
                input.trend_persistence, (weak_side == PositionSide::Long) ? "Long" : "Short"),
            .state = state_
        };
    }

    // Quality degradation — consider rebalance or exit
    if (quality < config_.min_pair_quality * 0.7) {
        double ratio = compute_hedge_ratio(input);
        if (std::abs(ratio - state_.long_leg.size / std::max(state_.short_leg.size, 1e-9)) > 0.15) {
            return PairDecision{
                .action = PairAction::RebalancePair,
                .hedge_ratio = ratio,
                .urgency = 0.3,
                .reason = std::format("Quality degraded: quality={:.2f}, rebalance ratio={:.2f}",
                    quality, ratio),
                .state = state_
            };
        }
    }

    return PairDecision{.action = PairAction::None, .state = state_};
}

// ============================================================
// TransitionPair: asymmetric during regime transition
// ============================================================

PairDecision PairLifecycleEngine::evaluate_transition(const PairMarketInput& input) {
    double close_score = compute_pair_close_score(input);

    // Emergency
    if (input.vpin_toxic && state_.net_pair_pnl_bps < config_.emergency_loss_threshold_bps) {
        return PairDecision{
            .action = PairAction::CloseBothEmergency,
            .urgency = 1.0,
            .reason = "Emergency: toxic flow during transition pair",
            .state = state_
        };
    }

    // Regime resolved to directional — promote strong leg
    if (input.regime_stability > 0.65 && std::abs(input.trend_persistence) > 0.35) {
        return PairDecision{
            .action = PairAction::PromoteToRunner,
            .urgency = 0.5,
            .reason = std::format("Transition resolved: trend={:.2f}, stability={:.2f}",
                input.trend_persistence, input.regime_stability),
            .state = state_
        };
    }

    // Regime resolved to compression — upgrade to compression pair
    if (input.regime_stability > 0.65 && std::abs(input.trend_persistence) < 0.15) {
        return PairDecision{
            .action = PairAction::OpenCompressionPair,
            .hedge_ratio = compute_hedge_ratio(input),
            .urgency = 0.3,
            .reason = "Transition→Compression: regime stabilized in range",
            .state = state_
        };
    }

    // Profit take
    if (close_score > config_.pair_profit_floor_bps) {
        return PairDecision{
            .action = PairAction::CloseBothProfit,
            .urgency = std::clamp(close_score / 15.0, 0.4, 0.85),
            .reason = std::format("Transition profit: close_score={:.1f}bps", close_score),
            .state = state_
        };
    }

    return PairDecision{.action = PairAction::None, .state = state_};
}

// ============================================================
// BreakoutShield: directional + protective leg
// ============================================================

PairDecision PairLifecycleEngine::evaluate_breakout_shield(const PairMarketInput& input) {
    // Emergency
    if (input.vpin_toxic && state_.net_pair_pnl_bps < config_.emergency_loss_threshold_bps) {
        return PairDecision{
            .action = PairAction::CloseBothEmergency,
            .urgency = 1.0,
            .reason = "Emergency: toxic flow during breakout shield",
            .state = state_
        };
    }

    // Breakout confirmed — promote to runner (remove protection)
    if (std::abs(input.trend_persistence) > 0.55 && input.p_shock < 0.10 &&
        input.regime_stability > 0.55) {
        return PairDecision{
            .action = PairAction::PromoteToRunner,
            .urgency = 0.4,
            .reason = std::format("Breakout confirmed: trend={:.2f}, shock_prob={:.2f}",
                input.trend_persistence, input.p_shock),
            .state = state_
        };
    }

    // Breakout failed — close both
    if (std::abs(input.trend_persistence) < 0.10 && input.regime_stability < 0.40) {
        double close_score = compute_pair_close_score(input);
        if (close_score > -5.0) {  // Close if loss is manageable
            return PairDecision{
                .action = PairAction::CloseBothProfit,
                .urgency = 0.6,
                .reason = std::format("Breakout failed: trend={:.2f}, close_score={:.1f}bps",
                    input.trend_persistence, close_score),
                .state = state_
            };
        }
    }

    // Shock probability increased — strengthen protective leg
    if (input.p_shock > config_.breakout_max_shock_prob * 1.5) {
        double new_ratio = std::clamp(0.20 + input.p_shock * 0.6, 0.25, 0.50);
        return PairDecision{
            .action = PairAction::RebalancePair,
            .hedge_ratio = new_ratio,
            .urgency = 0.5,
            .reason = std::format("Shield strengthen: p_shock={:.2f}→ratio={:.2f}",
                input.p_shock, new_ratio),
            .state = state_
        };
    }

    return PairDecision{.action = PairAction::None, .state = state_};
}

// ============================================================
// AsymmetricRunner: single strong leg
// ============================================================

PairDecision PairLifecycleEngine::evaluate_asymmetric_runner(const PairMarketInput& input) {
    // Trend reversal starting — need to re-shield
    if (input.cusum_regime_change && input.regime_stability < 0.40) {
        return PairDecision{
            .action = PairAction::OpenBreakoutShield,
            .hedge_ratio = std::clamp(0.20 + input.p_reversal * 0.4, 0.20, 0.35),
            .urgency = 0.7,
            .reason = std::format("Runner→Shield: regime_change, stability={:.2f}",
                input.regime_stability),
            .state = state_
        };
    }

    // Runner is losing edge — consider reversal
    if (input.exit_score_primary < -0.25 && input.p_reversal > 0.35) {
        return PairDecision{
            .action = PairAction::ReverseLeg,
            .urgency = 0.6,
            .reason = std::format("Runner reversal: exit_score={:.2f}, p_reversal={:.2f}",
                input.exit_score_primary, input.p_reversal),
            .state = state_
        };
    }

    return PairDecision{.action = PairAction::None, .state = state_};
}

// ============================================================
// Mode selection logic
// ============================================================

PairMode PairLifecycleEngine::select_pair_mode(const PairMarketInput& input) const {
    // Already have a hedge — don't open another pair
    if (input.has_hedge) return PairMode::NoPair;

    // Conditions don't warrant any pair
    if (input.primary_size <= 0.0) return PairMode::NoPair;

    // Check for breakout shield: strong directional edge + some shock risk
    if (std::abs(input.trend_persistence) > config_.breakout_min_trend_persistence &&
        input.p_shock > 0.08 &&
        input.p_shock < config_.breakout_max_shock_prob) {
        return PairMode::BreakoutShield;
    }

    // Check for compression pair: stable range conditions
    bool is_compression =
        input.regime_stability > config_.compression_min_regime_stability &&
        std::abs(input.trend_persistence) < 0.20 &&
        input.spread_bps < config_.compression_max_spread_bps &&
        input.depth_usd > config_.compression_min_depth_usd &&
        !input.vpin_toxic;

    if (is_compression) return PairMode::CompressionPair;

    // Check for transition pair: uncertain regime with reversal risk
    bool is_transition =
        input.p_reversal > config_.transition_min_p_reversal &&
        input.p_continue > 0.20 &&
        input.uncertainty < config_.transition_max_uncertainty &&
        input.primary_pnl_pct < -config_.hedge_trigger_loss_pct;

    if (is_transition) return PairMode::TransitionPair;

    return PairMode::NoPair;
}

// ============================================================
// Hedge ratio computation
// ============================================================

double PairLifecycleEngine::compute_hedge_ratio(const PairMarketInput& input) const {
    // Market-driven hedge ratio (no time inputs)
    double base = 0.70;

    // Regime break → full hedge
    if (input.cusum_regime_change && input.regime_stability < 0.35) {
        base = 1.0;
    }

    // Strong indicator consensus for reversal → increase hedge
    if (input.exit_score_primary < -0.15) {
        base = std::min(base + 0.20, 1.20);
    }

    // Toxic flow → at least full hedge
    if (input.vpin_toxic) {
        base = std::max(base, 1.0);
    }

    // Expensive funding for primary side → reduce hedge
    double effective_funding = (input.primary_side == PositionSide::Long) ?
        input.funding_rate : -input.funding_rate;
    if (std::abs(effective_funding) > 0.001) {
        base *= 0.80;
    }

    // Thin liquidity → limit hedge size
    if (input.depth_usd < 1000.0) {
        base = std::min(base, 0.80);
    }

    return std::clamp(base, 0.20, 1.20);
}

// ============================================================
// Pair quality computation
// ============================================================

double PairLifecycleEngine::compute_pair_quality(const PairMarketInput& input) const {
    // Pair quality degrades with adverse conditions
    double quality = 1.0;

    quality -= input.toxic_flow_score * 0.25;
    quality -= std::max(input.spread_bps - 15.0, 0.0) / 100.0;  // Spread penalty
    quality -= (input.depth_usd < 1000.0) ? 0.15 : 0.0;
    quality -= input.uncertainty * 0.15;
    quality += input.regime_stability * 0.10;

    // Funding drag penalty
    double funding_drag = std::abs(input.funding_rate) * 3.0 * 365.0 * 100.0;  // Annualized %
    quality -= std::min(funding_drag / 100.0, 0.20);

    return std::clamp(quality, 0.0, 1.0);
}

// ============================================================
// Pair close score: net_pnl - costs - risks
// ============================================================

double PairLifecycleEngine::compute_pair_close_score(const PairMarketInput& input) const {
    double round_trip_fees_bps = input.taker_fee_pct * 100.0 * 2.0;  // 2 legs to close (one per side)
    double slippage_bps = input.spread_bps * 0.5;  // Half spread as slippage estimate

    double score = state_.net_pair_pnl_bps
        - round_trip_fees_bps
        - slippage_bps
        - state_.funding_drag_bps;

    // Regime flip penalty
    if (input.cusum_regime_change) score -= 3.0;

    // Toxic flow penalty
    if (input.vpin_toxic) score -= 5.0;

    return score;
}

// ============================================================
// State update from market data
// ============================================================

void PairLifecycleEngine::update_state(const PairMarketInput& input) {
    state_.symbol = input.symbol;
    state_.mode = mode_;

    if (input.primary_size > 0.0) {
        auto& leg = (input.primary_side == PositionSide::Long) ?
            state_.long_leg : state_.short_leg;
        leg.active = true;
        leg.size = input.primary_size;
        leg.unrealized_pnl_bps = input.primary_pnl_pct * 100.0;
        leg.continuation_score = input.exit_score_primary;
    }

    if (input.has_hedge && input.hedge_size > 0.0) {
        auto& leg = (input.primary_side == PositionSide::Long) ?
            state_.short_leg : state_.long_leg;
        leg.active = true;
        leg.size = input.hedge_size;
        leg.unrealized_pnl_bps = input.hedge_pnl_pct * 100.0;
        leg.continuation_score = input.exit_score_hedge;
    }

    state_.net_pair_pnl_bps =
        state_.long_leg.unrealized_pnl_bps + state_.short_leg.unrealized_pnl_bps;
    state_.gross_notional =
        (state_.long_leg.size + state_.short_leg.size) * input.mid_price;
    state_.net_exposure =
        (state_.long_leg.size - state_.short_leg.size) * input.mid_price;
    // Signed funding drag: positive means the primary side pays, negative means
    // it receives a credit.  Long pays when funding_rate > 0; short pays when < 0.
    double signed_funding_drag =
        (input.primary_side == PositionSide::Long)
            ?  input.funding_rate * 100.0
            : -input.funding_rate * 100.0;
    state_.funding_drag_bps = signed_funding_drag;
    state_.pair_quality = compute_pair_quality(input);
}

// ============================================================
// Notifications
// ============================================================

void PairLifecycleEngine::notify_pair_opened(PairMode mode,
    const PairLeg& long_leg, const PairLeg& short_leg)
{
    mode_ = mode;
    state_.long_leg = long_leg;
    state_.short_leg = short_leg;
    state_.mode = mode;

    logger_->info("PairLifecycle",
        std::format("Pair opened: mode={}, L={:.4f}@{:.2f}, S={:.4f}@{:.2f}",
            to_string(mode), long_leg.size, long_leg.entry_price,
            short_leg.size, short_leg.entry_price),
        {});
}

void PairLifecycleEngine::notify_pair_closed(double net_pnl_bps) {
    logger_->info("PairLifecycle",
        std::format("Pair closed: mode={}, net_pnl={:.1f}bps",
            to_string(mode_), net_pnl_bps),
        {});

    mode_ = PairMode::NoPair;
    state_ = PairState{};
}

void PairLifecycleEngine::notify_leg_trimmed(PositionSide trimmed_side) {
    mode_ = PairMode::AsymmetricRunner;
    state_.mode = PairMode::AsymmetricRunner;

    if (trimmed_side == PositionSide::Long) {
        state_.long_leg = PairLeg{};
    } else {
        state_.short_leg = PairLeg{};
    }

    logger_->info("PairLifecycle",
        std::format("Leg trimmed: {} → AsymmetricRunner",
            (trimmed_side == PositionSide::Long) ? "Long" : "Short"),
        {});
}

void PairLifecycleEngine::reset() {
    mode_ = PairMode::NoPair;
    state_ = PairState{};
    if (hedge_pair_mgr_) hedge_pair_mgr_->reset();
}

} // namespace tb::pipeline
