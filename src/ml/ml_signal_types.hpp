#pragma once

/**
 * @file ml_signal_types.hpp
 * @brief Unified signal contract for all ML components.
 *
 * Defines MlSignalSnapshot — a single struct that captures the output of every
 * ML component (entropy filter, liquidation cascade detector, correlation
 * monitor, microstructure fingerprinter, Bayesian adapter, Thompson sampler)
 * evaluated at one point in time.  The pipeline fills this struct once per tick
 * and passes it downstream so consumers never need to query components
 * individually.
 */

#include <algorithm>
#include <cstdint>
#include <string>

namespace tb::ml {

// ─── Component health ────────────────────────────────────────────────────────

/// Runtime health of an individual ML component.
enum class MlComponentHealth {
    Healthy,    ///< Fully operational
    WarmingUp,  ///< Collecting initial samples, output is tentative
    Stale,      ///< Has not received data within expected window
    Degraded,   ///< Producing output but with reduced confidence
    Failed      ///< Unable to produce meaningful output
};

/// Per-component liveness / readiness metadata.
struct MlComponentStatus {
    MlComponentHealth health{MlComponentHealth::WarmingUp};
    int64_t last_update_ns{0};   ///< Timestamp of last on_tick (nanoseconds)
    int samples_processed{0};    ///< Total ticks consumed since start
    int warmup_remaining{0};     ///< Ticks until warmup complete; 0 = ready

    /// True only when the component is fully healthy.
    bool is_ready() const { return health == MlComponentHealth::Healthy; }

    /// True when the component can contribute useful (if tentative) output.
    bool is_usable() const {
        return health == MlComponentHealth::Healthy ||
               (health == MlComponentHealth::WarmingUp && samples_processed > 0);
    }
};

// ─── Unified snapshot ────────────────────────────────────────────────────────

/// Consolidated output of every ML component evaluated at a single point in
/// time.  Fill the per-component fields, then call compute_aggregates() to
/// derive the combined risk multiplier, block flag, and overall health.
struct MlSignalSnapshot {
    int64_t computed_at_ns{0};

    // ── Entropy / noise gating ───────────────────────────────────────────
    double composite_entropy{0.0};        ///< [0,1]
    double signal_quality{1.0};           ///< [0,1] = 1 - entropy
    bool is_noisy{false};
    MlComponentStatus entropy_status;

    // ── Liquidation cascade hazard ───────────────────────────────────────
    double cascade_probability{0.0};      ///< [0,1]
    bool cascade_imminent{false};
    int cascade_direction{0};             ///< +1 up, -1 down, 0 unknown
    MlComponentStatus cascade_status;

    // ── Cross-asset correlation ──────────────────────────────────────────
    double avg_correlation{0.0};
    bool correlation_break{false};
    double correlation_risk_multiplier{1.0}; ///< [0,1]
    MlComponentStatus correlation_status;

    // ── Microstructure edge ──────────────────────────────────────────────
    double fingerprint_edge{0.0};         ///< >0 favorable, <0 unfavorable
    double fingerprint_win_rate{0.5};
    int fingerprint_sample_count{0};
    MlComponentStatus fingerprint_status;

    // ── Bayesian adapted parameters ──────────────────────────────────────
    double adapted_conviction_threshold{0.3};
    double adapted_atr_stop_mult{2.0};
    MlComponentStatus bayesian_status;

    // ── Entry timing (Thompson) ──────────────────────────────────────────
    int recommended_wait_periods{0};      ///< 0 = enter now, -1 = skip
    double entry_confidence{0.5};         ///< α/(α+β) of chosen arm
    MlComponentStatus thompson_status;

    // ── Aggregate assessments ────────────────────────────────────────────

    /// Combined risk multiplier from all components [0,1].
    /// Computed as: correlation_risk * cascade_penalty * noise_penalty.
    double combined_risk_multiplier{1.0};

    /// Should trading be blocked based on ML signals?
    bool should_block_trading{false};

    /// Human-readable reason if trading is blocked.
    std::string block_reason;

    /// Overall health: worst of all component healths.
    MlComponentHealth overall_health{MlComponentHealth::WarmingUp};

    /// Is the ML layer ready for production trading?
    bool is_ready_for_trading() const {
        return overall_health == MlComponentHealth::Healthy && !should_block_trading;
    }

    /// Compute aggregate fields from component-level data.
    ///
    /// Threshold rationale (USDT-M perpetual futures, scalping):
    /// - cascade_probability > 0.4: Kyle (1985) — informed trading probability
    ///   above 40% indicates significant adverse selection pressure.
    /// - signal_quality < 0.15: Shannon entropy > 0.85 normalized means the
    ///   market microstructure is near-random; Cont (2001) shows mean-reversion
    ///   signals degrade below 15% signal-to-noise ratio.
    /// - cascade_imminent risk *= 0.3: reduces position risk to 30% — consistent
    ///   with VaR-based risk frameworks under extreme tail events (3σ+).
    /// - noisy risk *= 0.7: moderate 30% risk reduction under high entropy —
    ///   proportional to typical Sharpe degradation in noisy regimes.
    void compute_aggregates() {
        // Combined risk: multiplicative model preserves independence of factors.
        double risk = correlation_risk_multiplier;
        if (cascade_imminent)
            risk *= 0.3;
        else if (cascade_probability > 0.4)
            risk *= (1.0 - cascade_probability * 0.5);
        if (is_noisy) risk *= 0.7;
        combined_risk_multiplier = std::clamp(risk, 0.0, 1.0);

        // Block trading: hard gates for extreme conditions only.
        should_block_trading = false;
        block_reason.clear();
        if (cascade_imminent) {
            should_block_trading = true;
            block_reason = "liquidation_cascade_imminent";
        } else if (signal_quality < 0.15) {
            should_block_trading = true;
            block_reason = "signal_quality_too_low";
        }

        // Overall health = worst component
        auto worst = [](MlComponentHealth a, MlComponentHealth b) {
            return static_cast<int>(a) > static_cast<int>(b) ? a : b;
        };
        overall_health = MlComponentHealth::Healthy;
        for (auto s : {entropy_status.health, cascade_status.health,
                       correlation_status.health, fingerprint_status.health,
                       bayesian_status.health, thompson_status.health}) {
            overall_health = worst(overall_health, s);
        }
    }
};

} // namespace tb::ml
