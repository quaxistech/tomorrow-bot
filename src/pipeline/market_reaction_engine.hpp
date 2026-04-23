#pragma once

#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include "regime/regime_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "logging/logger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace tb::pipeline {

// ═══════════════════════════════════════════════════════════════════════════════
// UNIFIED MARKET STATE VECTOR
// Single source of truth for all decision-making: entry, hold, exit, hedge.
// Built once per tick, consumed by all downstream components.
// ═══════════════════════════════════════════════════════════════════════════════
struct MarketStateVector {
    // ── Microstructure ──
    double spread_bps{0.0};
    double bid_depth_5_notional{0.0};
    double ask_depth_5_notional{0.0};
    double vpin{0.0};
    bool vpin_toxic{false};
    double top_of_book_churn{0.0};
    double cancel_burst_intensity{0.0};
    double queue_depletion_bid{0.0};
    double queue_depletion_ask{0.0};
    double refill_asymmetry{0.0};
    double adverse_selection_bps{0.0};

    // ── Volatility ──
    double atr_14{0.0};
    double atr_pct{0.0};
    double realized_vol_short{0.0};
    double realized_vol_long{0.0};
    double vol_ratio{1.0};

    // ── Regime ──
    regime::DetailedRegime regime{regime::DetailedRegime::Undefined};
    double regime_stability{0.5};
    double regime_confidence{0.5};
    bool cusum_regime_change{false};

    // ── Trend ──
    double momentum_5{0.0};
    double momentum_20{0.0};
    bool momentum_valid{false};
    double rsi_14{50.0};
    double macd_histogram{0.0};
    double ema_slow{0.0};
    double ema_fast{0.0};
    double adx{0.0};
    double htf_trend{0.0};
    bool htf_valid{false};

    // ── Funding ──
    double funding_rate{0.0};

    // ── Time-of-Day context ──
    int session_hour_utc{0};
    double tod_volatility_mult{1.0};
    double tod_volume_mult{1.0};
    double tod_alpha_score{0.0};
    bool tod_valid{false};

    // ── Uncertainty ──
    double uncertainty_aggregate{0.5};
    uncertainty::UncertaintyAction recommended_action{uncertainty::UncertaintyAction::Normal};
    double size_multiplier{1.0};
    double threshold_multiplier{1.0};

    // ── Data quality ──
    bool is_feed_fresh{true};
    double data_quality{1.0};

    // ── Derived composite scores (computed by build_state) ──
    double liquidity_score{0.5};
    double toxicity_score{0.0};
    double microstructure_quality{0.5};

    // ── Price ──
    double mid_price{0.0};
};

// ═══════════════════════════════════════════════════════════════════════════════
// THREE-PROBABILITY MODEL
// P(continue) + P(reversal) + P(shock) + P(mean_revert) = 1.0
// ═══════════════════════════════════════════════════════════════════════════════
struct MarketProbabilities {
    double p_continue{0.50};
    double p_reversal{0.25};
    double p_shock{0.05};
    double p_mean_revert{0.20};

    bool valid() const {
        double sum = p_continue + p_reversal + p_shock + p_mean_revert;
        return std::abs(sum - 1.0) < 0.01;
    }
};

enum class MarketAction {
    Hold,
    Close,
    Hedge,
    Reverse,
    ReduceSize,
};

struct ActionEV {
    MarketAction action{MarketAction::Hold};
    double expected_value{0.0};
    double risk_adjusted_ev{0.0};
    std::string reason;
};

// ═══════════════════════════════════════════════════════════════════════════════
// MARKET DECISION — full output for existing position
// ═══════════════════════════════════════════════════════════════════════════════
struct MarketDecision {
    MarketProbabilities probs;
    std::array<ActionEV, 5> action_evs;
    MarketAction recommended{MarketAction::Hold};
    double confidence{0.5};
    std::string explanation;

    bool uncertainty_gated{false};
    double effective_size_mult{1.0};
    double effective_threshold_mult{1.0};

    double funding_drag_bps{0.0};
    bool funding_adverse{false};
};

// ═══════════════════════════════════════════════════════════════════════════════
// ENTRY QUALITY — evaluation for potential new entry
// ═══════════════════════════════════════════════════════════════════════════════
struct EntryQuality {
    double confidence{0.5};
    double size_multiplier{1.0};
    double threshold_multiplier{1.0};
    bool vetoed{false};
    std::string veto_reason;
    MarketProbabilities probs;
};

// ═══════════════════════════════════════════════════════════════════════════════
// MARKET REACTION ENGINE — Phase 4
//
// Unified market-state evaluation:
// 1. build_state() → MarketStateVector (once per tick)
// 2. evaluate() → MarketDecision (for existing positions)
// 3. evaluate_entry() → EntryQuality (for potential entries)
//
// All live decisions reduce to this evaluation. Entry, hold, hedge, exit
// use the same probability model and market state.
// ═══════════════════════════════════════════════════════════════════════════════
class MarketReactionEngine {
public:
    explicit MarketReactionEngine(std::shared_ptr<logging::ILogger> logger);

    MarketStateVector build_state(
        const features::FeatureSnapshot& snapshot,
        const regime::RegimeSnapshot& regime,
        const uncertainty::UncertaintySnapshot& uncertainty,
        double funding_rate,
        double htf_trend,
        bool htf_valid,
        bool is_feed_fresh);

    MarketDecision evaluate(
        const MarketStateVector& state,
        PositionSide current_side,
        double unrealized_pnl_pct,
        double exit_score);

    EntryQuality evaluate_entry(
        const MarketStateVector& state,
        PositionSide intended_side,
        double signal_strength);

private:
    MarketProbabilities estimate_probabilities(
        const MarketStateVector& state,
        PositionSide side) const;

    void compute_action_evs(
        MarketDecision& decision,
        const MarketStateVector& state,
        PositionSide side,
        double unrealized_pnl_pct) const;

    void apply_uncertainty_gating(
        MarketDecision& decision,
        const MarketStateVector& state) const;

    void compute_funding_impact(
        MarketDecision& decision,
        const MarketStateVector& state,
        PositionSide side) const;

    static bool is_momentum_aligned(const MarketStateVector& state, PositionSide side);

    static MarketProbabilities softmax_probs(
        double lo_continue, double lo_reversal,
        double lo_shock, double lo_mean_revert);

    std::shared_ptr<logging::ILogger> logger_;
};

inline const char* to_string(MarketAction a) {
    switch (a) {
    case MarketAction::Hold:       return "Hold";
    case MarketAction::Close:      return "Close";
    case MarketAction::Hedge:      return "Hedge";
    case MarketAction::Reverse:    return "Reverse";
    case MarketAction::ReduceSize: return "ReduceSize";
    }
    return "Unknown";
}

} // namespace tb::pipeline
