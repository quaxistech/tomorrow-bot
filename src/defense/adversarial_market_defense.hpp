#pragma once

#include "adversarial_defense/adversarial_defense.hpp"
#include "config/config_types.hpp"
#include "features/feature_snapshot.hpp"
#include "order_book/order_book_types.hpp"

namespace tb::defense {

using AdversarialMarketDefense = adversarial::AdversarialMarketDefense;

[[nodiscard]] inline std::string to_string(order_book::BookQuality quality) {
    switch (quality) {
        case order_book::BookQuality::Valid:         return "Valid";
        case order_book::BookQuality::Stale:         return "Stale";
        case order_book::BookQuality::Desynced:      return "Desynced";
        case order_book::BookQuality::Resyncing:     return "Resyncing";
        case order_book::BookQuality::Uninitialized: return "Uninitialized";
    }
    return "Unknown";
}

[[nodiscard]] inline adversarial::DefenseConfig make_defense_config(
    const config::AdversarialDefenseConfig& cfg) {
    return adversarial::DefenseConfig{
        .enabled = cfg.enabled,
        .fail_closed_on_invalid_data = cfg.fail_closed_on_invalid_data,
        .auto_cooldown_on_veto = cfg.auto_cooldown_on_veto,
        .auto_cooldown_severity = cfg.auto_cooldown_severity,
        .spread_explosion_threshold_bps = cfg.spread_explosion_threshold_bps,
        .spread_normal_bps = cfg.spread_normal_bps,
        .min_liquidity_depth = cfg.min_liquidity_depth,
        .book_imbalance_threshold = cfg.book_imbalance_threshold,
        .book_instability_threshold = cfg.book_instability_threshold,
        .toxic_flow_ratio_threshold = cfg.toxic_flow_ratio_threshold,
        .aggressive_flow_threshold = cfg.aggressive_flow_threshold,
        .vpin_toxic_threshold = cfg.vpin_toxic_threshold,
        .cooldown_duration_ms = cfg.cooldown_duration_ms,
        .post_shock_cooldown_ms = cfg.post_shock_cooldown_ms,
        .max_market_data_age_ns = cfg.max_market_data_age_ns,
        .max_confidence_reduction = cfg.max_confidence_reduction,
        .max_threshold_expansion = cfg.max_threshold_expansion,
        .compound_threat_factor = cfg.compound_threat_factor,
        .cooldown_severity_scale = cfg.cooldown_severity_scale,
        .recovery_duration_ms = cfg.recovery_duration_ms,
        .recovery_confidence_floor = cfg.recovery_confidence_floor,
        .spread_velocity_threshold_bps_per_sec = cfg.spread_velocity_threshold_bps_per_sec,
        // --- Adaptive baseline ---
        .baseline_alpha = cfg.baseline_alpha,
        .baseline_warmup_ticks = cfg.baseline_warmup_ticks,
        .z_score_spread_threshold = cfg.z_score_spread_threshold,
        .z_score_depth_threshold = cfg.z_score_depth_threshold,
        .z_score_ratio_threshold = cfg.z_score_ratio_threshold,
        .baseline_stale_reset_ms = cfg.baseline_stale_reset_ms,
        // --- Threat memory ---
        .threat_memory_alpha = cfg.threat_memory_alpha,
        .threat_memory_residual_factor = cfg.threat_memory_residual_factor,
        .threat_escalation_ticks = cfg.threat_escalation_ticks,
        .threat_escalation_boost = cfg.threat_escalation_boost,
        // --- Depth asymmetry ---
        .depth_asymmetry_threshold = cfg.depth_asymmetry_threshold,
        // --- Cross-signal amplification ---
        .cross_signal_amplification = cfg.cross_signal_amplification,
        // --- v4: Percentile scoring ---
        .percentile_window_size = cfg.percentile_window_size,
        .percentile_severity_threshold = cfg.percentile_severity_threshold,
        // --- v4: Correlation matrix ---
        .correlation_alpha = cfg.correlation_alpha,
        .correlation_breakdown_threshold = cfg.correlation_breakdown_threshold,
        // --- v4: Multi-timeframe ---
        .baseline_halflife_fast_ms = cfg.baseline_halflife_fast_ms,
        .baseline_halflife_medium_ms = cfg.baseline_halflife_medium_ms,
        .baseline_halflife_slow_ms = cfg.baseline_halflife_slow_ms,
        .timeframe_divergence_threshold = cfg.timeframe_divergence_threshold,
        // --- v4: Hysteresis ---
        .hysteresis_enter_severity = cfg.hysteresis_enter_severity,
        .hysteresis_exit_severity = cfg.hysteresis_exit_severity,
        .hysteresis_confidence_penalty = cfg.hysteresis_confidence_penalty,
        // --- v4: Event sourcing ---
        .audit_log_max_size = cfg.audit_log_max_size,
    };
}

[[nodiscard]] inline adversarial::MarketCondition build_market_condition(
    const features::FeatureSnapshot& snapshot) {
    return adversarial::MarketCondition{
        .symbol = snapshot.symbol,
        .spread_bps = snapshot.microstructure.spread_bps,
        .book_imbalance = snapshot.microstructure.book_imbalance_5,
        .bid_depth = snapshot.microstructure.bid_depth_5_notional,
        .ask_depth = snapshot.microstructure.ask_depth_5_notional,
        .book_instability = snapshot.microstructure.book_instability,
        .buy_sell_ratio = snapshot.microstructure.buy_sell_ratio,
        .aggressive_flow = snapshot.microstructure.aggressive_flow,
        .vpin = snapshot.microstructure.vpin,
        .vpin_valid = snapshot.microstructure.vpin_valid,
        .spread_valid = snapshot.microstructure.spread_valid,
        .liquidity_valid = snapshot.microstructure.liquidity_valid,
        .imbalance_valid = snapshot.microstructure.book_imbalance_valid,
        .instability_valid = snapshot.microstructure.instability_valid,
        .flow_valid = snapshot.microstructure.trade_flow_valid,
        .book_valid = snapshot.book_quality == order_book::BookQuality::Valid,
        .market_data_fresh = snapshot.execution_context.is_feed_fresh,
        .market_data_age_ns = snapshot.market_data_age_ns.get(),
        .book_state = to_string(snapshot.book_quality),
        .timestamp = snapshot.computed_at,
    };
}

} // namespace tb::defense
