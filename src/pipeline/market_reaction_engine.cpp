#include "pipeline/market_reaction_engine.hpp"
#include <numeric>

namespace tb::pipeline {

MarketReactionEngine::MarketReactionEngine(std::shared_ptr<logging::ILogger> logger)
    : logger_(std::move(logger)) {}

// ═══════════════════════════════════════════════════════════════════════════════
// BUILD STATE — assemble unified market state from all data sources
// ═══════════════════════════════════════════════════════════════════════════════
MarketStateVector MarketReactionEngine::build_state(
    const features::FeatureSnapshot& snap,
    const regime::RegimeSnapshot& regime,
    const uncertainty::UncertaintySnapshot& unc,
    double funding_rate,
    double htf_trend,
    bool htf_valid,
    bool is_feed_fresh) {

    MarketStateVector s;
    s.mid_price = snap.mid_price.get();

    // ── Microstructure ──
    if (snap.microstructure.spread_valid) {
        s.spread_bps = snap.microstructure.spread_bps;
    }
    if (snap.microstructure.liquidity_valid) {
        s.bid_depth_5_notional = snap.microstructure.bid_depth_5_notional;
        s.ask_depth_5_notional = snap.microstructure.ask_depth_5_notional;
    }
    if (snap.microstructure.vpin_valid) {
        s.vpin = snap.microstructure.vpin;
        s.vpin_toxic = snap.microstructure.vpin_toxic;
    }
    if (snap.microstructure.event_features_valid) {
        s.top_of_book_churn = snap.microstructure.top_of_book_churn;
        s.cancel_burst_intensity = snap.microstructure.cancel_burst_intensity;
        s.queue_depletion_bid = snap.microstructure.queue_depletion_bid;
        s.queue_depletion_ask = snap.microstructure.queue_depletion_ask;
        s.refill_asymmetry = snap.microstructure.refill_asymmetry;
    }
    if (snap.microstructure.execution_feedback_valid) {
        s.adverse_selection_bps = snap.microstructure.adverse_selection_bps;
    }

    // ── Volatility ──
    if (snap.technical.atr_valid) {
        s.atr_14 = snap.technical.atr_14;
        s.atr_pct = (s.mid_price > 0.0) ? (s.atr_14 / s.mid_price * 100.0) : 0.0;
    }
    if (snap.technical.volatility_valid) {
        s.realized_vol_short = snap.technical.volatility_5;
        s.realized_vol_long = snap.technical.volatility_20;
        s.vol_ratio = (s.realized_vol_long > 1e-9)
            ? (s.realized_vol_short / s.realized_vol_long) : 1.0;
    }

    // ── Regime ──
    s.regime = regime.detailed;
    s.regime_stability = regime.stability;
    s.regime_confidence = regime.confidence;
    if (snap.technical.cusum_valid) {
        s.cusum_regime_change = snap.technical.cusum_regime_change;
    }

    // ── Trend indicators ──
    if (snap.technical.momentum_valid) {
        s.momentum_5 = snap.technical.momentum_5;
        s.momentum_20 = snap.technical.momentum_20;
        s.momentum_valid = true;
    }
    s.rsi_14 = snap.technical.rsi_14;
    s.macd_histogram = snap.technical.macd_histogram;
    if (snap.technical.ema_valid) {
        s.ema_slow = snap.technical.ema_50;
        s.ema_fast = snap.technical.ema_20;
    }
    s.adx = snap.technical.adx;
    s.htf_trend = htf_trend;
    s.htf_valid = htf_valid;

    // ── Funding ──
    s.funding_rate = funding_rate;

    // ── Time-of-Day ──
    if (snap.technical.tod_valid) {
        s.session_hour_utc = snap.technical.session_hour_utc;
        s.tod_volatility_mult = snap.technical.tod_volatility_mult;
        s.tod_volume_mult = snap.technical.tod_volume_mult;
        s.tod_alpha_score = snap.technical.tod_alpha_score;
        s.tod_valid = true;
    }

    // ── Uncertainty ──
    s.uncertainty_aggregate = unc.aggregate_score;
    s.recommended_action = unc.recommended_action;
    s.size_multiplier = unc.size_multiplier;
    s.threshold_multiplier = unc.threshold_multiplier;

    // ── Data quality ──
    s.is_feed_fresh = is_feed_fresh;
    s.data_quality = regime.explanation.data_quality_score;

    // ── Derived composite scores ──
    // Liquidity score: 0=stress, 1=deep
    double total_depth = s.bid_depth_5_notional + s.ask_depth_5_notional;
    s.liquidity_score = std::clamp(total_depth / 50000.0, 0.0, 1.0);
    if (s.spread_bps > 20.0) {
        s.liquidity_score *= std::max(0.2, 1.0 - (s.spread_bps - 20.0) / 50.0);
    }

    // Toxicity score: 0=clean, 1=toxic
    s.toxicity_score = 0.0;
    if (s.vpin_toxic) s.toxicity_score += 0.40;
    s.toxicity_score += std::clamp(s.adverse_selection_bps / 5.0, 0.0, 0.30);
    s.toxicity_score += std::clamp(s.cancel_burst_intensity, 0.0, 0.15);
    s.toxicity_score += std::clamp(s.top_of_book_churn, 0.0, 0.15);
    s.toxicity_score = std::clamp(s.toxicity_score, 0.0, 1.0);

    // Microstructure quality: composite of spread + depth + stability
    s.microstructure_quality = 0.0;
    s.microstructure_quality += std::clamp(1.0 - s.spread_bps / 15.0, 0.0, 0.33);
    s.microstructure_quality += std::clamp(s.liquidity_score * 0.33, 0.0, 0.33);
    s.microstructure_quality += std::clamp((1.0 - s.toxicity_score) * 0.34, 0.0, 0.34);

    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PROBABILITY ESTIMATION — log-odds model with signal contributions
// ═══════════════════════════════════════════════════════════════════════════════
MarketProbabilities MarketReactionEngine::estimate_probabilities(
    const MarketStateVector& state,
    PositionSide side) const {

    // Base log-odds (prior): continue~55%, reversal~20%, shock~5%, mean_revert~20%
    double lo_c = 0.20;
    double lo_r = -1.40;
    double lo_s = -2.95;
    double lo_m = -1.40;

    // ── Momentum alignment ──
    if (state.momentum_valid) {
        if (is_momentum_aligned(state, side)) {
            lo_c += 0.35;
            lo_r -= 0.20;
        } else {
            lo_c -= 0.25;
            lo_r += 0.30;
        }
    }

    // ── Regime stability ──
    double stab_dev = state.regime_stability - 0.5;
    lo_c += stab_dev * 1.2;
    lo_r -= stab_dev * 0.8;
    lo_m -= stab_dev * 0.4;

    // ── CUSUM regime change ──
    if (state.cusum_regime_change) {
        lo_r += 0.60;
        lo_c -= 0.40;
        lo_s += 0.15;
    }

    // ── RSI extremity ──
    double rsi_ext = 0.0;
    if (state.rsi_14 > 75.0) rsi_ext = (state.rsi_14 - 75.0) / 25.0;
    else if (state.rsi_14 < 25.0) rsi_ext = (25.0 - state.rsi_14) / 25.0;
    lo_r += rsi_ext * 0.40;
    lo_m += rsi_ext * 0.35;
    lo_c -= rsi_ext * 0.20;

    // ── VPIN / toxicity ──
    if (state.vpin_toxic) {
        lo_s += 0.50;
        lo_c -= 0.15;
    }
    lo_s += state.toxicity_score * 0.40;

    // ── Spread stress ──
    if (state.spread_bps > 15.0) {
        double stress = std::min((state.spread_bps - 15.0) / 30.0, 1.0);
        lo_s += stress * 0.40;
        lo_c -= stress * 0.15;
    }

    // ── Liquidity ──
    lo_s -= (state.liquidity_score - 0.5) * 0.50;
    lo_c += (state.liquidity_score - 0.5) * 0.15;

    // ── HTF trend alignment ──
    // BUG-S8-11: htf_valid=true does not guarantee htf_trend is finite.
    // NaN > 0.0 and NaN < 0.0 both return false → silently trades counter-trend.
    if (state.htf_valid && std::isfinite(state.htf_trend)) {
        bool aligned = (side == PositionSide::Long && state.htf_trend > 0.0)
                    || (side == PositionSide::Short && state.htf_trend < 0.0);
        double mag = std::abs(state.htf_trend);
        if (aligned) {
            lo_c += mag * 0.35;
        } else {
            lo_r += mag * 0.30;
            lo_c -= mag * 0.20;
        }
    }

    // ── ADX ──
    if (state.adx > 25.0) {
        lo_c += 0.20;
        lo_m -= 0.25;
    } else if (state.adx < 15.0) {
        lo_m += 0.25;
        lo_c -= 0.15;
    }

    // ── Volatility expansion ──
    if (state.vol_ratio > 1.5) {
        lo_s += 0.20;
        lo_m -= 0.15;
    }

    // ── Time-of-Day ──
    if (state.tod_valid) {
        lo_c += state.tod_alpha_score * 0.20;
        if (state.tod_volume_mult < 0.6) {
            lo_m += 0.15;
            lo_c -= 0.10;
        }
    }

    // ── Data quality ──
    if (!state.is_feed_fresh || state.data_quality < 0.5) {
        lo_c -= 0.30;
        lo_s += 0.20;
    }

    // ── MACD ──
    bool macd_aligned = (side == PositionSide::Long && state.macd_histogram > 0.0)
                     || (side == PositionSide::Short && state.macd_histogram < 0.0);
    if (macd_aligned) lo_c += 0.15;
    else lo_r += 0.10;

    // BUG-S24-01: apply uncertainty penalty when no directional signals are active.
    // Without momentum, regime change, or extreme RSI in a low-ADX environment,
    // the prior alone is too optimistic about continuation probability.
    {
        const bool rsi_neutral = (state.rsi_14 >= 25.0 && state.rsi_14 <= 75.0);
        const bool adx_low = (state.adx < 20.0);
        if (!state.momentum_valid && !state.cusum_regime_change && rsi_neutral && adx_low) {
            lo_c -= 0.20;
            lo_s += 0.25;
        }
    }

    return softmax_probs(lo_c, lo_r, lo_s, lo_m);
}

// ═══════════════════════════════════════════════════════════════════════════════
// EV COMPUTATION — expected value for each possible action
// ═══════════════════════════════════════════════════════════════════════════════
void MarketReactionEngine::compute_action_evs(
    MarketDecision& decision,
    const MarketStateVector& state,
    PositionSide side,
    double unrealized_pnl_pct) const {

    const auto& p = decision.probs;
    double atr_move = std::max(state.atr_pct, 0.01);

    // Transaction costs
    constexpr double kTakerFeePct = 0.06;
    double half_spread_pct = state.spread_bps / 20000.0 * 100.0;
    double one_way_cost = half_spread_pct + kTakerFeePct;
    double round_trip_cost = 2.0 * one_way_cost;

    // Funding drag per 8h window
    bool funding_against = (side == PositionSide::Long && state.funding_rate > 0)
                        || (side == PositionSide::Short && state.funding_rate < 0);
    double funding_pct = std::abs(state.funding_rate) * 100.0;
    double signed_funding = funding_against ? funding_pct : -funding_pct;

    // Phase C: Replaced time_decay with market-driven continuation_quality.
    // Quality of continuation is based on market state, not position age.
    double momentum_alignment = 0.5;
    if (state.momentum_valid) {
        bool aligned = (side == PositionSide::Long && state.momentum_5 > 0.0)
                    || (side == PositionSide::Short && state.momentum_5 < 0.0);
        momentum_alignment = aligned ? std::clamp(std::abs(state.momentum_5) * 5.0, 0.5, 1.0) : 0.2;
    }
    double continuation_quality =
        0.45 * p.p_continue
      + 0.15 * momentum_alignment
      + 0.20 * state.regime_stability
      + 0.20 * (1.0 - std::clamp(state.spread_bps / 30.0, 0.0, 1.0));
    continuation_quality = std::clamp(continuation_quality, 0.1, 1.0);

    // ── Hold ──
    double hold_gain = p.p_continue * atr_move * 0.6 * continuation_quality;
    double hold_loss_rev = p.p_reversal * atr_move * 0.7;
    double hold_loss_shock = p.p_shock * atr_move * 2.0;
    double hold_mr = p.p_mean_revert * atr_move * 0.1;
    // signed_funding is already in pct per 8h period; use directly so funding
    // drag is comparable in magnitude to the ATR-based gain/loss terms.
    double hold_ev = hold_gain - hold_loss_rev - hold_loss_shock + hold_mr
                   - signed_funding;

    // ── Close ──
    double close_ev = unrealized_pnl_pct - one_way_cost;

    // ── Hedge ──
    double hedge_open_cost = one_way_cost;
    double hedge_protection = p.p_reversal * atr_move * 0.5
                            + p.p_shock * atr_move * 1.5;
    double hedge_drag = p.p_continue * std::abs(signed_funding) * 2.0 * 0.1;
    double hedge_ev = hedge_protection - hedge_open_cost - hedge_drag;

    // ── Reverse ──
    double reverse_cost = round_trip_cost + one_way_cost;
    double reverse_gain = p.p_reversal * atr_move * 1.2;
    double reverse_loss = p.p_continue * atr_move * 0.8;
    double reverse_ev = reverse_gain - reverse_loss - reverse_cost;

    // ── ReduceSize ──
    double reduce_ev = 0.5 * close_ev + 0.5 * hold_ev;

    // Risk-adjusted EV: penalize downside
    auto risk_adj = [](double ev, double worst_case) -> double {
        return ev - 0.30 * std::max(worst_case, 0.0);
    };

    decision.action_evs[0] = {MarketAction::Hold, hold_ev,
        risk_adj(hold_ev, hold_loss_rev + hold_loss_shock), "Hold position"};
    decision.action_evs[1] = {MarketAction::Close, close_ev,
        risk_adj(close_ev, one_way_cost),
        unrealized_pnl_pct >= 0 ? "Take profit" : "Cut loss"};
    decision.action_evs[2] = {MarketAction::Hedge, hedge_ev,
        risk_adj(hedge_ev, hedge_open_cost + hedge_drag), "Open hedge leg"};
    decision.action_evs[3] = {MarketAction::Reverse, reverse_ev,
        risk_adj(reverse_ev, reverse_cost + reverse_loss), "Reverse position"};
    decision.action_evs[4] = {MarketAction::ReduceSize, reduce_ev,
        risk_adj(reduce_ev, (hold_loss_rev + hold_loss_shock) * 0.5), "Reduce exposure"};

    // Select best action by risk-adjusted EV
    auto best = std::max_element(
        decision.action_evs.begin(), decision.action_evs.end(),
        [](const ActionEV& a, const ActionEV& b) {
            return a.risk_adjusted_ev < b.risk_adjusted_ev;
        });
    decision.recommended = best->action;

    // Confidence = separation between best and second-best
    std::array<double, 5> revs;
    for (int i = 0; i < 5; i++) revs[i] = decision.action_evs[i].risk_adjusted_ev;
    std::sort(revs.begin(), revs.end(), std::greater<>());
    double gap = revs[0] - revs[1];
    decision.confidence = std::clamp(gap / (atr_move * 0.5 + 0.01), 0.0, 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// EVALUATE — full analysis for existing position
// ═══════════════════════════════════════════════════════════════════════════════
MarketDecision MarketReactionEngine::evaluate(
    const MarketStateVector& state,
    PositionSide current_side,
    double unrealized_pnl_pct,
    double exit_score) {

    MarketDecision decision;

    // Step 1: estimate scenario probabilities
    decision.probs = estimate_probabilities(state, current_side);

    // Step 2: compute EVs for all actions
    compute_action_evs(decision, state, current_side,
                       unrealized_pnl_pct);

    // Step 3: apply uncertainty gating
    apply_uncertainty_gating(decision, state);

    // Step 4: compute funding impact
    compute_funding_impact(decision, state, current_side);

    // Build explanation
    decision.explanation =
        "P(c)=" + std::to_string(decision.probs.p_continue).substr(0, 5)
        + " P(r)=" + std::to_string(decision.probs.p_reversal).substr(0, 5)
        + " P(s)=" + std::to_string(decision.probs.p_shock).substr(0, 5)
        + " → " + to_string(decision.recommended)
        + " (conf=" + std::to_string(decision.confidence).substr(0, 4) + ")";

    return decision;
}

// ═══════════════════════════════════════════════════════════════════════════════
// EVALUATE ENTRY — quality assessment for potential new position
// ═══════════════════════════════════════════════════════════════════════════════
EntryQuality MarketReactionEngine::evaluate_entry(
    const MarketStateVector& state,
    PositionSide intended_side,
    double signal_strength) {

    EntryQuality eq;

    // Compute probabilities for the intended side
    eq.probs = estimate_probabilities(state, intended_side);

    // Entry confidence based on P(continue) and signal strength
    eq.confidence = eq.probs.p_continue * 0.6 + signal_strength * 0.4;

    // Inherit uncertainty engine multipliers
    eq.size_multiplier = state.size_multiplier;
    eq.threshold_multiplier = state.threshold_multiplier;

    // ── Veto conditions ──
    if (state.recommended_action == uncertainty::UncertaintyAction::NoTrade) {
        eq.vetoed = true;
        eq.veto_reason = "Uncertainty NoTrade";
        return eq;
    }
    if (!state.is_feed_fresh) {
        eq.vetoed = true;
        eq.veto_reason = "Stale data feed";
        return eq;
    }
    if (eq.probs.p_shock > 0.15) {
        eq.vetoed = true;
        eq.veto_reason = "P(shock)=" + std::to_string(eq.probs.p_shock).substr(0, 5);
        return eq;
    }

    // ── Low continuation → tighten threshold ──
    if (eq.probs.p_continue < 0.40) {
        eq.threshold_multiplier *= 1.30;
    }

    // ── High reversal → reduce size ──
    if (eq.probs.p_reversal > 0.35) {
        eq.size_multiplier *= 0.50;
    }

    // ── Funding against entry side ──
    bool funding_against =
        (intended_side == PositionSide::Long && state.funding_rate > 0.0005)
        || (intended_side == PositionSide::Short && state.funding_rate < -0.0005);
    if (funding_against) {
        eq.size_multiplier *= 0.80;
        eq.threshold_multiplier *= 1.15;
    }

    // ── Low-alpha ToD hours → tighten ──
    if (state.tod_valid && state.tod_alpha_score < -0.3) {
        eq.threshold_multiplier *= 1.20;
    }

    // ── High uncertainty → reduce ──
    if (state.uncertainty_aggregate > 0.7) {
        eq.size_multiplier *= std::max(0.5, 1.0 - (state.uncertainty_aggregate - 0.7));
    }

    return eq;
}

// ═══════════════════════════════════════════════════════════════════════════════
// UNCERTAINTY GATING — modulate decision under epistemic uncertainty
// ═══════════════════════════════════════════════════════════════════════════════
void MarketReactionEngine::apply_uncertainty_gating(
    MarketDecision& decision,
    const MarketStateVector& state) const {

    decision.effective_size_mult = state.size_multiplier;
    decision.effective_threshold_mult = state.threshold_multiplier;

    if (state.uncertainty_aggregate > 0.70) {
        decision.uncertainty_gated = true;

        // Under high uncertainty, bias toward risk reduction
        double unc_penalty = (state.uncertainty_aggregate - 0.70) * 3.0;
        decision.action_evs[0].risk_adjusted_ev -= unc_penalty * 0.1;  // Hold
        decision.action_evs[4].risk_adjusted_ev += unc_penalty * 0.05; // ReduceSize
        decision.action_evs[1].risk_adjusted_ev += unc_penalty * 0.03; // Close

        // Recompute best action
        auto best = std::max_element(
            decision.action_evs.begin(), decision.action_evs.end(),
            [](const ActionEV& a, const ActionEV& b) {
                return a.risk_adjusted_ev < b.risk_adjusted_ev;
            });
        decision.recommended = best->action;
    }

    if (state.recommended_action == uncertainty::UncertaintyAction::ReducedSize) {
        decision.effective_size_mult *= 0.60;
    } else if (state.recommended_action == uncertainty::UncertaintyAction::HigherThreshold) {
        decision.effective_threshold_mult *= 1.50;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// FUNDING IMPACT — compute carry cost/benefit for position
// ═══════════════════════════════════════════════════════════════════════════════
void MarketReactionEngine::compute_funding_impact(
    MarketDecision& decision,
    const MarketStateVector& state,
    PositionSide side) const {

    bool against = (side == PositionSide::Long && state.funding_rate > 0)
                || (side == PositionSide::Short && state.funding_rate < 0);
    double annualized_bps = std::abs(state.funding_rate) * 3.0 * 365.0 * 10000.0;
    decision.funding_drag_bps = against ? annualized_bps : -annualized_bps;
    decision.funding_adverse = against && annualized_bps > 500.0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════════
bool MarketReactionEngine::is_momentum_aligned(
    const MarketStateVector& state, PositionSide side) {
    if (side == PositionSide::Long) return state.momentum_5 > 0.0001;
    return state.momentum_5 < -0.0001;
}

MarketProbabilities MarketReactionEngine::softmax_probs(
    double lo_c, double lo_r, double lo_s, double lo_m) {
    // Numerical stability: subtract max
    double mx = std::max({lo_c, lo_r, lo_s, lo_m});
    double ec = std::exp(lo_c - mx);
    double er = std::exp(lo_r - mx);
    double es = std::exp(lo_s - mx);
    double em = std::exp(lo_m - mx);
    double total = ec + er + es + em;

    MarketProbabilities p;
    p.p_continue = ec / total;
    p.p_reversal = er / total;
    p.p_shock = es / total;
    p.p_mean_revert = em / total;
    return p;
}

} // namespace tb::pipeline
