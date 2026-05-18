#include "uncertainty/uncertainty_engine.hpp"
#include "portfolio/portfolio_types.hpp"
#include "ml/ml_signal_types.hpp"
#include <algorithm>
#include <cmath>

namespace tb::uncertainty {

// ─── Serialization helpers (preserved for telemetry) ────────────────────────

std::string to_string(UncertaintyAction action) {
    switch (action) {
        case UncertaintyAction::Normal:          return "Normal";
        case UncertaintyAction::ReducedSize:     return "ReducedSize";
        case UncertaintyAction::HigherThreshold: return "HigherThreshold";
        case UncertaintyAction::NoTrade:         return "NoTrade";
    }
    return "Normal";
}

std::string to_string(ExecutionModeRecommendation mode) {
    switch (mode) {
        case ExecutionModeRecommendation::Normal:         return "Normal";
        case ExecutionModeRecommendation::Conservative:   return "Conservative";
        case ExecutionModeRecommendation::DefensiveOnly:  return "DefensiveOnly";
        case ExecutionModeRecommendation::HaltNewEntries: return "HaltNewEntries";
    }
    return "Normal";
}

// ─── Engine ─────────────────────────────────────────────────────────────────
//
// Scalping refactor (2026-05): the 9-dimensional uncertainty machine
// (regime / signal / data_quality / execution / portfolio / ml / correlation /
// transition / operational) was replaced with a deterministic 4-signal gate
// that targets the only failure modes a $15-account Bitget scalper actually
// suffers from:
//
//   1. Wide spread        — fee-margin gets eaten before any move.
//   2. Stale market feed  — entries on snapshot price ≠ live price.
//   3. Toxic VPIN flow    — informed traders on the other side.
//   4. Book instability   — pre-cascade conditions.
//
// Levels, multipliers and the cooldown mechanism are preserved at the type
// level for downstream compatibility, but their calculation is now a single
// pass over the four signals.

RuleBasedUncertaintyEngine::RuleBasedUncertaintyEngine(
    UncertaintyConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock)) {}

UncertaintySnapshot RuleBasedUncertaintyEngine::assess(
    const features::FeatureSnapshot& features,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world) {

    // Minimal v1 path: build neutral portfolio/ML snapshots and delegate.
    portfolio::PortfolioSnapshot empty_pf;
    ml::MlSignalSnapshot neutral_ml;
    return assess(features, regime, world, empty_pf, neutral_ml);
}

UncertaintySnapshot RuleBasedUncertaintyEngine::assess(
    const features::FeatureSnapshot& features,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world,
    const portfolio::PortfolioSnapshot& /*portfolio*/,
    const ml::MlSignalSnapshot& ml_signals) {

    std::lock_guard lk(mutex_);
    const int64_t now_ns = clock_ ? static_cast<int64_t>(clock_->now().get()) : 0;
    const std::string& sym_key = features.symbol.get();
    auto& state = states_[sym_key];

    UncertaintySnapshot out;
    out.symbol = features.symbol;
    out.computed_at = clock_ ? clock_->now() : Timestamp(0);

    const auto& m = features.microstructure;
    const auto& exec_ctx = features.execution_context;

    // ── 1. Per-signal sub-scores ──
    // Each sub-score is in [0, 1]; raw values are clamped to keep the
    // aggregate bounded even when upstream feeds emit garbage.
    auto clamp01 = [](double x) { return std::clamp(x, 0.0, 1.0); };

    // B23.1: Spread — saturation at 25 bps (typical 1m crypto upper bound).
    // Если нужна параметризация → добавить в UncertaintyConfig.
    constexpr double kSpreadSatBps = 25.0;
    const double spread_u = (m.spread_valid && std::isfinite(m.spread_bps))
        ? clamp01(m.spread_bps / kSpreadSatBps) : 0.5;

    // B23.2: stale-feed weight 0.8 — сильный сигнал но не Extreme (0.85+).
    // Если нужно усилить gate → UncertaintyConfig.stale_feed_severity.
    constexpr double kStaleFeedSeverity = 0.8;
    const double data_u = exec_ctx.is_feed_fresh ? 0.0 : kStaleFeedSeverity;

    // B23.3: VPIN — toxic flag → 0.85 (high but not absolute);
    // линейный non-toxic mapping от 0.4 (lower toxicity threshold).
    // Easley-Lopez de Prado 2012 → эмпирические пороги.
    constexpr double kVpinToxicSeverity = 0.85;
    constexpr double kVpinNonToxicLowerThr = 0.4;
    constexpr double kVpinNonToxicRange = 0.4;
    double vpin_u = 0.0;
    if (m.vpin_valid) {
        if (m.vpin_toxic) vpin_u = kVpinToxicSeverity;
        else vpin_u = clamp01((m.vpin - kVpinNonToxicLowerThr) / kVpinNonToxicRange);
    }

    // Book instability: pre-cascade conditions.
    const double instability_u = clamp01(m.book_instability);

    // Regime confirmation deficit (acts as a soft signal): low regime
    // confidence is a small contributor; we no longer treat it as a
    // first-class dimension.
    const double regime_u = std::isfinite(regime.confidence)
        ? clamp01(1.0 - regime.confidence) * 0.5 : 0.25;

    // World adversarial / disruption — fragility from the slim world model.
    const double world_u = world.fragility.valid
        ? clamp01(world.fragility.value) : 0.3;

    // ML-side disruption: cascade probability + correlation regime break.
    double ml_u = 0.0;
    ml_u = std::max(ml_u, clamp01(ml_signals.cascade_probability));
    // B23.4: correlation_break — single-flag impulse, ~0.7 (high but not max).
    constexpr double kCorrelationBreakSeverity = 0.7;
    if (ml_signals.correlation_break) ml_u = std::max(ml_u, kCorrelationBreakSeverity);

    // ── 2. Aggregate ──
    // B23.5: weighting 70% hard signals (gating) + 30% soft signals (modifier).
    // Этот split — фундаментальный design choice; параметризовать через config
    // если нужно ослабить gating priority.
    constexpr double kHardSignalWeight = 0.7;
    constexpr double kSoftSignalWeight = 0.3;
    const double hard_max = std::max({spread_u, data_u, vpin_u, instability_u});
    const double soft_avg = (regime_u + world_u + ml_u) / 3.0;
    double raw_score = kHardSignalWeight * hard_max + kSoftSignalWeight * soft_avg;
    raw_score = clamp01(raw_score);

    // EMA smoothing on persistent score; spike score is the raw delta.
    const double alpha = std::clamp(config_.ema_alpha, 0.01, 1.0);
    state.ema_score = (1.0 - alpha) * state.ema_score + alpha * raw_score;
    out.persistent_score = state.ema_score;
    out.spike_score = std::max(0.0, raw_score - state.ema_score);
    out.aggregate_score = raw_score;

    // ── 3. Level + hysteresis ──
    UncertaintyLevel new_level;
    if (raw_score >= config_.threshold_high)          new_level = UncertaintyLevel::Extreme;
    else if (raw_score >= config_.threshold_moderate) new_level = UncertaintyLevel::High;
    else if (raw_score >= config_.threshold_low)      new_level = UncertaintyLevel::Moderate;
    else                                              new_level = UncertaintyLevel::Low;

    // Simple hysteresis: require a small margin to step down.
    if (new_level < state.prev_level) {
        const double down_bias = config_.hysteresis_down;
        if (state.prev_level == UncertaintyLevel::Extreme && raw_score > config_.threshold_high - down_bias)
            new_level = UncertaintyLevel::Extreme;
        else if (state.prev_level == UncertaintyLevel::High && raw_score > config_.threshold_moderate - down_bias)
            new_level = UncertaintyLevel::High;
    }
    out.level = new_level;

    // ── 4. Action / sizing / threshold multipliers ──
    switch (new_level) {
        case UncertaintyLevel::Low:
            out.recommended_action = UncertaintyAction::Normal;
            out.size_multiplier = 1.0;
            out.threshold_multiplier = 1.0;
            break;
        case UncertaintyLevel::Moderate:
            out.recommended_action = UncertaintyAction::ReducedSize;
            out.size_multiplier = 0.7;
            out.threshold_multiplier = 1.1;
            break;
        case UncertaintyLevel::High:
            out.recommended_action = UncertaintyAction::HigherThreshold;
            out.size_multiplier = 0.4;
            out.threshold_multiplier = 1.5;
            break;
        case UncertaintyLevel::Extreme:
            out.recommended_action = UncertaintyAction::NoTrade;
            out.size_multiplier = config_.size_floor;
            out.threshold_multiplier = std::min(config_.threshold_ceiling, 2.0);
            break;
    }

    // ── 5. Dimensions (preserved for telemetry; populated minimally) ──
    out.dimensions.regime_uncertainty       = regime_u;
    out.dimensions.signal_uncertainty       = vpin_u;
    out.dimensions.data_quality_uncertainty = data_u;
    out.dimensions.execution_uncertainty    = spread_u;
    out.dimensions.portfolio_uncertainty    = 0.0;
    out.dimensions.ml_uncertainty           = ml_u;
    out.dimensions.correlation_uncertainty  = ml_signals.correlation_break ? 0.7 : 0.0;
    out.dimensions.transition_uncertainty   = regime_u;
    out.dimensions.operational_uncertainty  = data_u;

    // ── 6. Execution mode ──
    if (new_level == UncertaintyLevel::Extreme) {
        out.execution_mode = ExecutionModeRecommendation::HaltNewEntries;
    } else if (new_level == UncertaintyLevel::High) {
        out.execution_mode = ExecutionModeRecommendation::DefensiveOnly;
    } else if (new_level == UncertaintyLevel::Moderate) {
        out.execution_mode = ExecutionModeRecommendation::Conservative;
    } else {
        out.execution_mode = ExecutionModeRecommendation::Normal;
    }

    // ── 7. Cooldown (consecutive-extreme counter) ──
    if (new_level == UncertaintyLevel::Extreme) {
        ++state.consecutive_extreme;
        state.consecutive_high = 0;
    } else if (new_level == UncertaintyLevel::High) {
        ++state.consecutive_high;
        state.consecutive_extreme = 0;
    } else {
        state.consecutive_extreme = 0;
        state.consecutive_high = 0;
    }
    if (state.consecutive_extreme >= config_.consecutive_extreme_for_cooldown) {
        state.cooldown_until_ns = now_ns + config_.cooldown_duration_ns;
        ++diagnostics_.cooldown_activations;
    }
    if (state.cooldown_until_ns > now_ns) {
        out.cooldown.active = true;
        out.cooldown.remaining_ns = state.cooldown_until_ns - now_ns;
        out.cooldown.decay_factor = 0.5;
        out.cooldown.trigger_reason = "consecutive_extreme";
        // Force action to NoTrade while cooldown is active.
        out.recommended_action = UncertaintyAction::NoTrade;
        out.execution_mode = ExecutionModeRecommendation::HaltNewEntries;
    }

    // ── 8. Top drivers (lightweight; just the three dominant ones) ──
    struct D { const char* name; double value; };
    const std::array<D, 4> drivers = {{
        {"spread",       spread_u},
        {"data",         data_u},
        {"vpin",         vpin_u},
        {"instability",  instability_u},
    }};
    std::vector<UncertaintyDriver> top;
    top.reserve(3);
    auto sorted = drivers;
    std::sort(sorted.begin(), sorted.end(), [](const D& a, const D& b) {
        return a.value > b.value;
    });
    for (size_t i = 0; i < std::min<size_t>(3, sorted.size()); ++i) {
        UncertaintyDriver d;
        d.dimension = sorted[i].name;
        d.raw_value = sorted[i].value;
        d.contribution = sorted[i].value;
        top.push_back(std::move(d));
    }
    out.top_drivers = std::move(top);

    state.prev_level = new_level;
    state.last_assess_ns = now_ns;
    snapshots_[sym_key] = out;

    ++diagnostics_.total_assessments;
    if (out.recommended_action == UncertaintyAction::NoTrade) ++diagnostics_.veto_count;
    diagnostics_.last_assessment = out.computed_at;
    // Rolling average over a small effective window.
    diagnostics_.avg_aggregate_score =
        0.95 * diagnostics_.avg_aggregate_score + 0.05 * raw_score;

    return out;
}

UncertaintyDiagnostics RuleBasedUncertaintyEngine::diagnostics() const {
    std::lock_guard lk(mutex_);
    return diagnostics_;
}

void RuleBasedUncertaintyEngine::reset_state() {
    std::lock_guard lk(mutex_);
    states_.clear();
    snapshots_.clear();
    diagnostics_ = UncertaintyDiagnostics{};
}

} // namespace tb::uncertainty
