#include "pipeline/exit_orchestrator.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace tb::pipeline {

PositionExitOrchestrator::PositionExitOrchestrator(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock)) {}

// ─── Main evaluate: runs all exit signals in priority order ───────────────

ExitDecision PositionExitOrchestrator::evaluate(const ExitContext& ctx) const {
    if (ctx.hedge_active) {
        // Hedge is managed separately — orchestrator doesn't override hedge manager
        return {};
    }

    // 1. Hard risk stops (cannot be deferred or overridden)
    if (auto d = check_fixed_capital_stop(ctx); d.should_exit) return d;
    if (auto d = check_price_stop(ctx); d.should_exit) return d;

    // 2. Toxic flow (emergency — overrides everything except hard stops)
    if (auto d = check_toxic_flow(ctx); d.should_exit) return d;

    // 2a. Stale data exit (safety — can't trade blind)
    if (auto d = check_stale_data_exit(ctx); d.should_exit) return d;

    // 3. Compute continuation state (used by all remaining signals)
    auto state = compute_continuation_state(ctx);

    // 3a. Market regime exit (CUSUM-confirmed regime change against position)
    if (auto d = check_market_regime_exit(ctx, state); d.should_exit) return d;

    // 3b. Funding carry exit (funding penalty makes hold unprofitable)
    if (auto d = check_funding_carry_exit(ctx); d.should_exit) {
        d.state = state;
        return d;
    }

    // 4. Trailing stop (Chandelier Exit)
    if (auto d = check_trailing_stop(ctx); d.should_exit) {
        d.state = state;
        return d;
    }

    // 5. Partial take-profit (market-aware)
    if (auto d = check_partial_tp(ctx, state); d.should_reduce) {
        d.state = state;
        return d;
    }

    // 6. Quick profit (gated by continuation value)
    if (auto d = check_quick_profit(ctx, state); d.should_exit) return d;

    // 7. Structural failure
    if (auto d = check_structural_failure(ctx); d.should_exit) {
        d.state = state;
        return d;
    }

    // 8. Liquidity deterioration
    if (auto d = check_liquidity_deterioration(ctx, state); d.should_exit) return d;

    // 9. Continuation value model (heart of the system — replaces time-based exits)
    if (auto d = check_continuation_value_exit(ctx, state); d.should_exit) return d;

    return {};
}

// ─── Hard stops ───────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_fixed_capital_stop(const ExitContext& ctx) const {
    if (ctx.total_capital <= 0.0) return {};
    double loss_pct = std::abs(std::min(ctx.unrealized_pnl, 0.0)) / ctx.total_capital * 100.0;
    if (loss_pct < ctx.max_loss_per_trade_pct) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 1.0;
    d.explanation = build_explanation(
        ExitSignalType::HardRiskStop,
        std::format("Capital loss {:.2f}% >= {:.2f}% limit", loss_pct, ctx.max_loss_per_trade_pct),
        {},
        "Position would need to recover to reduce loss below capital limit");
    return d;
}

ExitDecision PositionExitOrchestrator::check_price_stop(const ExitContext& ctx) const {
    if (ctx.entry_price <= 0.0) return {};

    bool is_long = (ctx.position_side == PositionSide::Long);
    double price_loss_pct = is_long
        ? (ctx.entry_price - ctx.current_price) / ctx.entry_price * 100.0
        : (ctx.current_price - ctx.entry_price) / ctx.entry_price * 100.0;

    if (price_loss_pct < ctx.price_stop_loss_pct) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 1.0;
    d.explanation = build_explanation(
        ExitSignalType::HardRiskStop,
        std::format("Price loss {:.2f}% >= {:.2f}% from entry", price_loss_pct, ctx.price_stop_loss_pct),
        {},
        "Price would need to retrace significantly to avoid stop");
    return d;
}

// ─── Trailing stop ────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_trailing_stop(const ExitContext& ctx) const {
    if (ctx.current_stop_level <= 0.0) return {};

    bool is_long = (ctx.position_side == PositionSide::Long);
    bool crossed = is_long
        ? (ctx.current_price <= ctx.current_stop_level)
        : (ctx.current_price >= ctx.current_stop_level);
    if (!crossed) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.9;

    auto signal = ctx.breakeven_activated
        ? ExitSignalType::TrailingStop
        : ExitSignalType::HardRiskStop; // pre-breakeven = protective ATR stop

    d.explanation = build_explanation(
        signal,
        std::format("Price {:.4f} crossed {} stop {:.4f} (trail_mult={:.2f})",
            ctx.current_price,
            ctx.breakeven_activated ? "trailing" : "protective ATR",
            ctx.current_stop_level, ctx.current_trail_mult),
        {},
        ctx.breakeven_activated
            ? "Stop level would need to be higher than current price"
            : "Position did not reach breakeven before ATR stop triggered");
    return d;
}

PositionExitOrchestrator::TrailingUpdate
PositionExitOrchestrator::update_trailing(const ExitContext& ctx) const {
    TrailingUpdate u;
    u.stop_level = ctx.current_stop_level;
    u.trail_mult = ctx.current_trail_mult;
    u.breakeven_activated = ctx.breakeven_activated;
    u.highest = ctx.highest_price_since_entry;
    u.lowest = ctx.lowest_price_since_entry;

    if (!ctx.atr_valid || ctx.atr_14 <= 0.0 || ctx.entry_price <= 0.0) return u;

    double atr = ctx.atr_14;
    double entry = ctx.entry_price;
    double price = ctx.current_price;
    bool is_long = (ctx.position_side == PositionSide::Long);

    // Dynamic ATR multiplier
    double base = ctx.atr_stop_multiplier;

    double adx_f = 1.0;
    if (ctx.adx > 30.0) adx_f = 0.85;
    else if (ctx.adx <= 20.0) adx_f = 1.15;

    double depth_f = 1.0;
    if (ctx.depth_usd < 500.0) depth_f = 1.3;
    else if (ctx.depth_usd < 2000.0) depth_f = 1.1;

    double spread_f = 1.0;
    if (ctx.spread_bps > 50.0) spread_f = 1.3;
    else if (ctx.spread_bps > 20.0) spread_f = 1.1;

    double bb_f = 1.0;
    if (ctx.bb_width > 0.05) bb_f = 1.2;
    else if (ctx.bb_width > 0.03) bb_f = 1.1;

    double pressure_f = 1.0;
    if (is_long && ctx.buy_pressure < 0.3) pressure_f = 0.8;
    if (!is_long && ctx.buy_pressure > 0.7) pressure_f = 0.8;

    u.trail_mult = std::clamp(
        base * adx_f * depth_f * spread_f * bb_f * pressure_f,
        0.8, 3.0);

    double min_fee_offset = entry * ctx.taker_fee_pct / 100.0 * 3.0;
    double fee_floor_in_atr = (atr > 0.0) ? (min_fee_offset / atr) : 100.0;
    double effective_be_threshold = std::max(ctx.breakeven_atr_threshold, fee_floor_in_atr * 1.5);

    if (is_long) {
        u.highest = std::max(ctx.highest_price_since_entry, price);
        double profit_in_atr = (price - entry) / atr;

        if (!ctx.breakeven_activated) {
            if (profit_in_atr >= effective_be_threshold) {
                u.breakeven_activated = true;
                double current_profit = price - entry;
                double be = entry + current_profit * 0.5;
                be = std::max(be, entry + min_fee_offset);
                be = std::max(be, entry + atr * 0.1);
                be = std::min(be, price - atr * 0.1);
                u.stop_level = be;
            } else if (u.stop_level <= 0.0) {
                double dist = std::max(u.trail_mult * atr, entry * 0.003);
                u.stop_level = entry - dist;
            }
        } else {
            double dist = std::max(u.trail_mult * atr, entry * 0.003);
            double new_stop = u.highest - dist;
            u.stop_level = std::max(ctx.current_stop_level, new_stop);
        }
    } else {
        u.lowest = std::min(ctx.lowest_price_since_entry, price);
        double profit_in_atr = (entry - price) / atr;

        if (!ctx.breakeven_activated) {
            if (profit_in_atr >= effective_be_threshold) {
                u.breakeven_activated = true;
                double current_profit = entry - price;
                double be = entry - current_profit * 0.5;
                be = std::min(be, entry - min_fee_offset);
                be = std::min(be, entry - atr * 0.1);
                be = std::max(be, price + atr * 0.1);
                u.stop_level = be;
            } else if (u.stop_level <= 0.0) {
                double dist = std::max(u.trail_mult * atr, entry * 0.003);
                u.stop_level = entry + dist;
            }
        } else {
            double dist = std::max(u.trail_mult * atr, entry * 0.003);
            double new_stop = u.lowest + dist;
            if (ctx.current_stop_level <= 0.0) {
                u.stop_level = new_stop;
            } else {
                u.stop_level = std::min(ctx.current_stop_level, new_stop);
            }
        }
    }

    return u;
}

// ─── Quick profit ─────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_quick_profit(
    const ExitContext& ctx, const ContinuationState& state) const {
    if (ctx.unrealized_pnl <= 0.0) return {};

    bool is_long = (ctx.position_side == PositionSide::Long);
    double notional = ctx.position_size * ctx.current_price;
    double round_trip_fee = notional * ctx.taker_fee_pct / 100.0 * 2.0;
    double min_profit = round_trip_fee * ctx.quick_profit_fee_multiplier;

    double min_profit_atr = 0.0;
    if (ctx.atr_valid && ctx.atr_14 > 0.0) {
        min_profit_atr = std::max(1.25, ctx.partial_tp_atr_threshold * 0.75);
        double profit_in_atr = is_long
            ? (ctx.current_price - ctx.entry_price) / ctx.atr_14
            : (ctx.entry_price - ctx.current_price) / ctx.atr_14;
        if (profit_in_atr < min_profit_atr) return {};

        min_profit = std::max(min_profit, ctx.position_size * ctx.atr_14 * min_profit_atr);
    }

    if (ctx.unrealized_pnl <= min_profit) return {};

    // Quick-profit is only valid when continuation has already turned meaningfully weak.
    // Mildly positive or only slightly negative continuation should be left to trailing/
    // partial-tp logic; otherwise winners get clipped near the fee floor.
    if (state.continuation_value > -0.05) return {};

    int adverse_confirms = 0;
    if (state.mean_reversion_hazard > 0.3) ++adverse_confirms;
    if (state.liquidity_deterioration > 0.3) ++adverse_confirms;
    if (state.toxic_flow_score > 0.3) ++adverse_confirms;
    if (state.edge_decay > 0.35) ++adverse_confirms;
    if (state.regime_transition_risk > 0.4) ++adverse_confirms;
    if (adverse_confirms < 1 && state.continuation_value > -0.12) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.7;
    d.state = state;
    d.explanation = build_explanation(
        ExitSignalType::QuickProfitHarvest,
        std::format("Profit {:.2f} > {:.2f} quick-profit floor, continuation_value={:.3f} confirms harvest",
            ctx.unrealized_pnl, min_profit, state.continuation_value),
        state,
        min_profit_atr > 0.0
            ? std::format("Would hold if continuation_value > -0.05 or profit < {:.2f}xATR",
                min_profit_atr)
            : "Would hold if continuation_value > -0.05");
    return d;
}

// ─── Partial TP (market-aware) ──────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_partial_tp(
    const ExitContext& ctx, const ContinuationState& state) const {
    if (ctx.partial_tp_taken) return {};
    if (!ctx.atr_valid || ctx.atr_14 <= 0.0) return {};

    double atr = ctx.atr_14;
    bool is_long = (ctx.position_side == PositionSide::Long);
    double profit_in_atr = is_long
        ? (ctx.current_price - ctx.entry_price) / atr
        : (ctx.entry_price - ctx.current_price) / atr;

    // Market-aware threshold: raise threshold when trend is strong and regime is stable
    // In a stable trending regime, let profits run further before taking partial
    double adaptive_threshold = ctx.partial_tp_atr_threshold;
    if (ctx.regime_stability > 0.7 && state.trend_persistence > 0.3) {
        // Strong trend + stable regime: raise threshold by 30% (let profits run)
        adaptive_threshold *= 1.3;
    } else if (ctx.regime_stability < 0.3 || ctx.uncertainty > 0.7) {
        // Unstable regime or high uncertainty: lower threshold by 20% (take profits sooner)
        adaptive_threshold *= 0.8;
    }

    if (profit_in_atr < adaptive_threshold) return {};

    double notional = ctx.position_size * ctx.current_price;
    double round_trip_fee = notional * ctx.taker_fee_pct / 100.0 * 2.0;
    if (ctx.unrealized_pnl < round_trip_fee * 1.5) return {};

    // Market-aware fraction: reduce more in high-uncertainty or weak-trend conditions
    double adaptive_fraction = ctx.partial_tp_fraction;
    if (state.continuation_value < 0.0) {
        // Continuation is negative — take more off the table
        adaptive_fraction = std::min(adaptive_fraction + 0.15, 0.75);
    } else if (state.continuation_value > 0.20 && ctx.regime_stability > 0.6) {
        // Strong continuation + stable regime — take less
        adaptive_fraction = std::max(adaptive_fraction - 0.10, 0.25);
    }

    ExitDecision d;
    d.should_reduce = true;
    d.reduce_fraction = adaptive_fraction;
    d.urgency = 0.6;
    d.explanation = build_explanation(
        ExitSignalType::PartialReduce,
        std::format("Profit {:.2f}×ATR >= {:.2f}×ATR adaptive threshold (base {:.2f}), "
            "reducing {:.0f}%, CV={:.3f}, stability={:.2f}",
            profit_in_atr, adaptive_threshold, ctx.partial_tp_atr_threshold,
            adaptive_fraction * 100.0, state.continuation_value, ctx.regime_stability),
        state,
        "Would hold full if profit < ATR threshold or continuation strong");
    return d;
}

// ─── Toxic flow ────────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_toxic_flow(const ExitContext& ctx) const {
    if (!ctx.vpin_toxic) return {};
    if (ctx.unrealized_pnl >= 0.0) return {}; // Don't exit profitable position on VPIN alone

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.95;
    d.explanation = build_explanation(
        ExitSignalType::ToxicFlowExit,
        "VPIN toxic flow detected while in loss",
        {},
        "Would hold if position were profitable or VPIN not toxic");
    return d;
}

// ─── Structural failure ────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_structural_failure(const ExitContext& ctx) const {
    if (ctx.unrealized_pnl_pct >= -0.5) return {};  // Only on meaningful losses

    bool is_long = (ctx.position_side == PositionSide::Long);
    bool ema_cross = is_long
        ? (ctx.ema_20 < ctx.ema_50)
        : (ctx.ema_20 > ctx.ema_50);

    bool momentum_against = is_long
        ? (ctx.macd_histogram < 0.0)
        : (ctx.macd_histogram > 0.0);

    if (!ema_cross || !momentum_against) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.8;
    d.explanation = build_explanation(
        ExitSignalType::StructuralFailure,
        std::format("EMA cross ({:.2f} vs {:.2f}) + MACD against ({:.6f}) with PnL {:.2f}%",
            ctx.ema_20, ctx.ema_50, ctx.macd_histogram, ctx.unrealized_pnl_pct),
        {},
        "Would hold if EMA structure supported position direction");
    return d;
}

// ─── Liquidity deterioration ───────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_liquidity_deterioration(
    const ExitContext& ctx, const ContinuationState& state) const {
    if (state.liquidity_deterioration < 0.7) return {};
    if (ctx.unrealized_pnl_pct >= -1.0) return {};

    bool is_long = (ctx.position_side == PositionSide::Long);
    double imb = is_long ? ctx.book_imbalance : -ctx.book_imbalance;
    if (imb > -0.3) return {};  // Book not significantly against us

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.75;
    d.state = state;
    d.explanation = build_explanation(
        ExitSignalType::LiquidityDeteriorationExit,
        std::format("Liquidity deterioration {:.2f} with book imbalance {:.2f} against position, PnL {:.2f}%",
            state.liquidity_deterioration, imb, ctx.unrealized_pnl_pct),
        state,
        "Would hold if book depth and imbalance supported position");
    return d;
}

// ─── Market Regime Exit ────────────────────────────────────────────────────
// Triggers when the market regime has confirmed a change that makes the
// current position direction unfavorable. Uses CUSUM regime change detection
// and regime stability/confidence from the regime engine.

ExitDecision PositionExitOrchestrator::check_market_regime_exit(
    const ExitContext& ctx, const ContinuationState& state) const {
    // Requires: regime stability dropped significantly AND CUSUM detected change
    if (ctx.regime_stability > 0.35) return {};  // Regime still reasonably stable
    if (!ctx.cusum_regime_change) return {};       // No structural break detected

    // Must be in a position that's against the new regime direction
    // Low confidence + low stability = confirmed transition
    bool regime_hostile = ctx.regime_confidence > 0.4 && ctx.regime_stability < 0.25;
    bool position_at_risk = ctx.unrealized_pnl_pct < 0.5;  // Not deeply profitable

    if (!regime_hostile && !position_at_risk) return {};

    // Additional confirmation: EMA cross or momentum reversal against position
    bool is_long = (ctx.position_side == PositionSide::Long);
    bool ema_against = is_long
        ? (ctx.ema_20 < ctx.ema_50 && ctx.ema_50 > 0.0)
        : (ctx.ema_20 > ctx.ema_50 && ctx.ema_50 > 0.0);
    bool macd_against = is_long
        ? (ctx.macd_histogram < 0.0)
        : (ctx.macd_histogram > 0.0);

    // Need at least one momentum confirmation
    if (!ema_against && !macd_against) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = std::clamp(0.7 + (0.35 - ctx.regime_stability), 0.7, 0.95);
    d.state = state;
    d.explanation = build_explanation(
        ExitSignalType::MarketRegimeExit,
        std::format("Regime transition: stability={:.2f}, confidence={:.2f}, CUSUM break, "
            "EMA_against={}, MACD_against={}, PnL={:.2f}%",
            ctx.regime_stability, ctx.regime_confidence,
            ema_against, macd_against, ctx.unrealized_pnl_pct),
        state,
        "Would hold if regime stability > 0.35 or no CUSUM break");
    return d;
}

// ─── Funding Carry Exit ────────────────────────────────────────────────────
// Exits when funding rate penalty makes continuation unprofitable.
// Funding is paid every 8 hours — annualized cost can be substantial.

ExitDecision PositionExitOrchestrator::check_funding_carry_exit(const ExitContext& ctx) const {
    bool is_long = (ctx.position_side == PositionSide::Long);
    double effective_funding = is_long ? ctx.funding_rate : -ctx.funding_rate;

    // Only exit if we're paying funding (positive = bad for our side)
    if (effective_funding <= 0.0) return {};

    // Annualized cost: funding × 3 (per day) × 365
    double annualized_pct = effective_funding * 100.0 * 3.0 * 365.0;

    // Threshold: exit if funding cost exceeds projected edge
    // At 0.05% per 8h → 54.75% annualized — clearly unprofitable
    // At 0.01% per 8h → 10.95% annualized — borderline
    // Use sliding threshold based on PnL state: losing positions more sensitive
    double threshold_annualized = (ctx.unrealized_pnl_pct < 0.0) ? 15.0 : 40.0;
    if (annualized_pct < threshold_annualized) return {};

    // Gate: projected funding drag must exceed current unrealized gain
    // to justify exit. This is market-driven (forecast) not time-driven.
    // Project drag over next funding interval (8h = 480 min)
    double projected_drag_pct = effective_funding * 100.0;
    double edge_remaining = std::max(ctx.unrealized_pnl_pct, 0.0);
    // If projected drag for single interval doesn't eat remaining edge, skip
    if (projected_drag_pct < edge_remaining * 0.5 && annualized_pct < 80.0) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = std::clamp(annualized_pct / 100.0, 0.5, 0.85);
    d.state = {};
    d.explanation.primary_signal = ExitSignalType::FundingCarryExit;
    d.explanation.category = ExitCategory::Alpha;
    d.explanation.primary_driver = std::format(
        "Funding carry unprofitable: rate={:.4f}% per 8h, annualized={:.1f}%, "
        "threshold={:.0f}%, projected_drag={:.4f}%, edge_remaining={:.2f}%",
        effective_funding * 100.0, annualized_pct, threshold_annualized,
        projected_drag_pct, edge_remaining);
    d.explanation.counterfactual = "Would hold if funding rate were negative (receiving) or below threshold";
    return d;
}

// ─── Stale Data Exit ───────────────────────────────────────────────────────
// Exits when market data feed is not fresh — cannot make informed decisions.

ExitDecision PositionExitOrchestrator::check_stale_data_exit(const ExitContext& ctx) const {
    if (ctx.is_feed_fresh) return {};

    // Only exit if position is at risk (in loss or thin market)
    bool at_risk = ctx.unrealized_pnl_pct < -0.5 || ctx.depth_usd < 1000.0;
    if (!at_risk) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.85;
    d.state = {};
    d.explanation.primary_signal = ExitSignalType::StaleDataExit;
    d.explanation.category = ExitCategory::StaleData;
    d.explanation.primary_driver = std::format(
        "Stale data exit: feed not fresh, PnL={:.2f}%, depth=${:.0f}",
        ctx.unrealized_pnl_pct, ctx.depth_usd);
    d.explanation.counterfactual = "Would hold if feed were fresh or position were profitable";
    return d;
}

// ─── Continuation Value Model ──────────────────────────────────────────────

ContinuationState PositionExitOrchestrator::compute_continuation_state(const ExitContext& ctx) const {
    ContinuationState s;
    bool is_long = (ctx.position_side == PositionSide::Long);

    // 1. Trend persistence: EMA alignment + momentum direction
    {
        double ema_gap = (ctx.ema_50 > 0.0)
            ? (ctx.ema_20 - ctx.ema_50) / ctx.ema_50
            : 0.0;
        double ema_signal = is_long ? ema_gap : -ema_gap;

        double macd_signal = is_long ? ctx.macd_histogram : -ctx.macd_histogram;
        double norm = (ctx.atr_valid && ctx.atr_14 > 0.0) ? ctx.atr_14 * 0.1 : 1.0;
        double macd_norm = std::clamp(macd_signal / norm, -1.0, 1.0);

        s.trend_persistence = std::clamp(ema_signal * 50.0 * 0.6 + macd_norm * 0.4, -1.0, 1.0);
    }

    // 2. Mean reversion hazard: RSI extremes
    {
        double rsi = ctx.rsi_14;
        if (is_long) {
            if (rsi > 75.0) s.mean_reversion_hazard = 0.9;
            else if (rsi > 65.0) s.mean_reversion_hazard = 0.4;
            else if (rsi > 55.0) s.mean_reversion_hazard = 0.1;
            else s.mean_reversion_hazard = 0.0;
        } else {
            if (rsi < 25.0) s.mean_reversion_hazard = 0.9;
            else if (rsi < 35.0) s.mean_reversion_hazard = 0.4;
            else if (rsi < 45.0) s.mean_reversion_hazard = 0.1;
            else s.mean_reversion_hazard = 0.0;
        }
    }

    // 3. Liquidity deterioration: spread + depth
    {
        double spread_risk = 0.0;
        if (ctx.spread_bps > 50.0) spread_risk = 0.8;
        else if (ctx.spread_bps > 30.0) spread_risk = 0.4;
        else if (ctx.spread_bps > 15.0) spread_risk = 0.15;

        double depth_risk = 0.0;
        if (ctx.depth_usd < 500.0) depth_risk = 0.8;
        else if (ctx.depth_usd < 2000.0) depth_risk = 0.3;

        s.liquidity_deterioration = std::min(spread_risk * 0.5 + depth_risk * 0.5, 1.0);
    }

    // 4. Toxic flow: VPIN
    s.toxic_flow_score = ctx.vpin_toxic ? 0.8 : 0.0;

    // 5. Queue/fill deterioration: book imbalance against position
    {
        double imb_against = is_long ? -ctx.book_imbalance : ctx.book_imbalance;
        s.queue_fill_deterioration = std::clamp(imb_against * 1.5, 0.0, 1.0);
    }

    // 6. Funding carry penalty
    {
        double effective_funding = is_long ? ctx.funding_rate : -ctx.funding_rate;
        // Positive = paying (bad), negative = receiving (good)
        if (effective_funding > 0.001) s.funding_carry_penalty = 0.7;
        else if (effective_funding > 0.0005) s.funding_carry_penalty = 0.3;
        else if (effective_funding > 0.0) s.funding_carry_penalty = 0.1;
        else s.funding_carry_penalty = 0.0;
    }

    // 7. Edge decay: realized vs unrealized
    //    If PnL was better earlier (drawdown from peak), edge is decaying
    {
        if (ctx.entry_price > 0.0) {
            double peak_pnl_pct = is_long
                ? (ctx.highest_price_since_entry - ctx.entry_price) / ctx.entry_price * 100.0
                : (ctx.entry_price - ctx.lowest_price_since_entry) / ctx.entry_price * 100.0;
            double current_pnl_pct = ctx.unrealized_pnl_pct;
            double decay = (peak_pnl_pct > 0.0)
                ? std::max(0.0, (peak_pnl_pct - current_pnl_pct) / std::max(peak_pnl_pct, 0.01))
                : 0.0;
            s.edge_decay = std::clamp(decay, 0.0, 1.0);
        }
    }

    // 8. Regime transition risk (now uses real regime engine data)
    s.regime_transition_risk = 1.0 - ctx.regime_stability;

    // 9. Exit confidence (from uncertainty engine — real aggregate score)
    s.exit_confidence = ctx.uncertainty;

    // 10. Cost of staying: net cost of maintaining the position
    //     Combines funding drag, slippage risk on exit, and exit liquidity cost.
    //     This converts the continuation value into a net expected value.
    {
        bool is_paying_funding = is_long
            ? (ctx.funding_rate > 0.0) : (ctx.funding_rate < 0.0);
        // Annualized funding drag: funding is paid every 8h (3× per day)
        // Convert to per-minute cost for comparison with hold duration
        double funding_drag = is_paying_funding
            ? std::abs(ctx.funding_rate) * 3.0 * 365.0  // annualized rate
            : 0.0;
        // Normalize: 0.1% funding = 10.95% annualized → scale to [0,0.5]
        double funding_cost = std::clamp(funding_drag / 0.20, 0.0, 0.5);

        // Slippage risk: estimated cost to exit at market
        double slippage_cost = std::clamp(ctx.estimated_slippage_bps / 30.0, 0.0, 0.3);

        // Exit liquidity cost: thin books increase exit cost
        double liq_cost = 0.0;
        if (ctx.depth_usd < 500.0) liq_cost = 0.3;
        else if (ctx.depth_usd < 2000.0) liq_cost = 0.1;

        s.cost_of_staying = std::clamp(
            funding_cost * 0.5 + slippage_cost * 0.3 + liq_cost * 0.2, 0.0, 1.0);
    }

    // Compute weighted net expected continuation value.
    // All market-driven components (no time-based factors).
    // Positive factors (reasons to hold):
    //   trend_persistence           weight 0.30
    // Negative factors (reasons to exit):
    //   mean_reversion_hazard       weight 0.12
    //   liquidity_deterioration     weight 0.10
    //   toxic_flow_score            weight 0.10
    //   queue_fill_deterioration    weight 0.08
    //   funding_carry_penalty       weight 0.06
    //   edge_decay                  weight 0.09
    //   regime_transition_risk      weight 0.08
    //   exit_confidence             weight 0.06
    //   cost_of_staying             weight 0.01
    // Total positive: 0.30, total negative: 0.70
    s.continuation_value =
        s.trend_persistence * 0.30
        - s.mean_reversion_hazard * 0.12
        - s.liquidity_deterioration * 0.10
        - s.toxic_flow_score * 0.10
        - s.queue_fill_deterioration * 0.08
        - s.funding_carry_penalty * 0.06
        - s.edge_decay * 0.09
        - s.regime_transition_risk * 0.08
        - s.exit_confidence * 0.06
        - s.cost_of_staying * 0.01;

    // Phase 4: Modulate continuation value using Market Reaction Engine probabilities.
    // P(continue) > 0.5 → boost hold incentive; P(reversal) high → increase exit pressure.
    // This ties the exit decision to the unified probability model.
    double p_continue_adj = (ctx.p_continue - 0.50) * 0.15;
    double p_reversal_adj = (ctx.p_reversal - 0.25) * 0.20;
    double p_shock_adj = (ctx.p_shock - 0.05) * 0.25;
    s.continuation_value += p_continue_adj - p_reversal_adj - p_shock_adj;

    return s;
}

ExitDecision PositionExitOrchestrator::check_continuation_value_exit(
    const ExitContext& ctx, const ContinuationState& state) const {
    // Phase C: Dynamic exit threshold based on regime stability and uncertainty.
    // Replaces static -0.02 and time-based hold_minutes < 2.0 guard.
    double exit_threshold = -0.08
        + 0.03 * std::clamp(ctx.regime_stability, 0.0, 1.0)
        - 0.04 * std::clamp(ctx.uncertainty, 0.0, 1.0);

    if (state.continuation_value > exit_threshold) return {};

    // Fee-aware anti-churn gate.
    // Continuation exit is an alpha exit, not a hard-risk kill switch. If the trade is
    // still inside the economic noise band, closing here only locks in taker fees and
    // microstructure noise. Let hard stops / trailing / structural exits handle danger;
    // continuation exit should engage only once the move is economically meaningful.
    double notional = ctx.position_size * ctx.current_price;
    double round_trip_fee = notional * ctx.taker_fee_pct / 100.0 * 2.0;
    double profit_activation = round_trip_fee * ctx.quick_profit_fee_multiplier;
    double shallow_loss_floor = -round_trip_fee * 1.5;
    bool inside_economic_dead_zone =
        ctx.unrealized_pnl > shallow_loss_floor &&
        ctx.unrealized_pnl < profit_activation;

    if (inside_economic_dead_zone) {
        return {};
    }

    // Require at least 2 independent bearish/bullish confirmations against current leg
    int bearish_confirms = 0;
    if (state.mean_reversion_hazard > 0.3) ++bearish_confirms;
    if (state.toxic_flow_score > 0.3) ++bearish_confirms;
    if (state.liquidity_deterioration > 0.3) ++bearish_confirms;
    if (state.regime_transition_risk > 0.3) ++bearish_confirms;
    if (state.edge_decay > 0.4) ++bearish_confirms;
    if (bearish_confirms < 2 && state.continuation_value > -0.15) return {};

    ExitDecision d;
    d.should_exit = true;
    d.state = state;
    d.urgency = std::clamp(-state.continuation_value * 2.0, 0.5, 1.0);

    // Find largest negative contributor for primary driver
    struct Factor { const char* name; double value; };
    Factor factors[] = {
        {"mean_reversion_hazard", state.mean_reversion_hazard * 0.12},
        {"liquidity_deterioration", state.liquidity_deterioration * 0.10},
        {"toxic_flow", state.toxic_flow_score * 0.10},
        {"book_pressure", state.queue_fill_deterioration * 0.08},
        {"funding_carry", state.funding_carry_penalty * 0.06},
        {"edge_decay", state.edge_decay * 0.09},
        {"regime_risk", state.regime_transition_risk * 0.08},
        {"uncertainty", state.exit_confidence * 0.06},
        {"cost_of_staying", state.cost_of_staying * 0.01},
    };

    double max_contrib = 0.0;
    const char* primary_name = "continuation_value_negative";
    for (const auto& f : factors) {
        if (f.value > max_contrib) {
            max_contrib = f.value;
            primary_name = f.name;
        }
    }

    // Build secondary drivers
    std::vector<ExitReasonComponent> secondaries;
    for (const auto& f : factors) {
        if (f.value > 0.01 && f.name != primary_name) {
            secondaries.push_back({f.name, f.value});
        }
    }
    secondaries.push_back({"trend_persistence", state.trend_persistence * 0.30});

    d.explanation.primary_signal = ExitSignalType::ContinuationValueExit;
    d.explanation.category = ExitCategory::Alpha;
    d.explanation.primary_driver = std::format(
        "Continuation value {:.3f} < {:.2f}: primary factor is {} ({:.3f})",
        state.continuation_value, exit_threshold, primary_name, max_contrib);
    d.explanation.secondary_drivers = std::move(secondaries);
    d.explanation.counterfactual = std::format(
        "Would hold if continuation_value > {:.2f}; "
        "trend_persistence={:.3f} insufficient to offset negatives",
        exit_threshold, state.trend_persistence);

    return d;
}

// ─── Explanation builder ───────────────────────────────────────────────────

ExitExplanation PositionExitOrchestrator::build_explanation(
    ExitSignalType signal,
    const std::string& primary,
    const ContinuationState& state,
    const std::string& counterfactual) const {

    ExitExplanation e;
    e.primary_signal = signal;
    e.category = exit_category(signal);
    e.primary_driver = primary;
    e.counterfactual = counterfactual;

    // Add state components as secondary drivers if state was computed
    if (state.continuation_value != 0.0 || state.trend_persistence != 0.0) {
        if (std::abs(state.trend_persistence) > 0.01)
            e.secondary_drivers.push_back({"trend_persistence", state.trend_persistence});
        if (state.edge_decay > 0.01)
            e.secondary_drivers.push_back({"edge_decay", state.edge_decay});
        if (state.toxic_flow_score > 0.01)
            e.secondary_drivers.push_back({"toxic_flow", state.toxic_flow_score});
    }

    return e;
}

} // namespace tb::pipeline
