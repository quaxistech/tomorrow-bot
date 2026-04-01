#pragma once

#include <string>
#include <unordered_map>

#include "regime/regime_types.hpp"

namespace tb::regime {

/// Stress policy: what the system should do in each stress regime
enum class StressAction {
    Allow,          ///< Normal trading allowed
    ReduceSize,     ///< Reduce position size (soft stress)
    RaiseThreshold, ///< Raise conviction thresholds (medium stress)
    BlockEntry,     ///< Block new entries, allow exits only (hard stress)
    HaltAll         ///< Halt all trading (anomaly / no-trade)
};

/// Policy for a specific regime — what downstream systems should enforce
struct RegimePolicy {
    StressAction action{StressAction::Allow};
    double size_cap{1.0};             ///< Max fraction of normal size [0,1]
    double threshold_multiplier{1.0}; ///< Multiplier for conviction thresholds
    bool allow_new_entries{true};
    bool allow_exits{true};
};

/// Thresholds for trend classification
struct TrendThresholds {
    double adx_strong{30.0};       ///< ADX above this = strong trend
    double adx_weak_min{18.0};     ///< ADX range for weak trend [min, max]
    double adx_weak_max{30.0};
    // ИСПРАВЛЕНИЕ: RSI=50 нейтрален, не бычий. Для StrongUptrend/Downtrend
    // требуется RSI >55 (bullish) или <45 (bearish).
    double rsi_trend_bias{55.0};   ///< RSI above = bullish bias, below = bearish
};

/// Thresholds for mean-reversion detection
struct MeanReversionThresholds {
    double rsi_overbought{70.0};
    double rsi_oversold{30.0};
    double adx_max{25.0};          ///< Max ADX for mean-reversion regime
};

/// Thresholds for volatility detection
struct VolatilityThresholds {
    double bb_bandwidth_expansion{0.06};
    double bb_bandwidth_compression{0.02};
    double atr_norm_expansion{0.02};
    double adx_compression_max{20.0};
};

/// Thresholds for anomaly / toxic flow / liquidity stress
struct StressThresholds {
    double rsi_extreme_high{85.0};
    double rsi_extreme_low{15.0};
    double obv_norm_extreme{2.0};
    double aggressive_flow_toxic{0.75};
    // ИСПРАВЛЕНИЕ: Micro-cap altcoins имеют спреды 50-150bps.
    // 15bps слишком низкий порог — ложные срабатывания ToxicFlow.
    double spread_toxic_bps{80.0};        ///< Макс спред для ToxicFlow детекции (micro-cap worst-case 150bps)
    double book_instability_threshold{0.6};
    double spread_stress_bps{30.0};
    double liquidity_ratio_stress{3.0};
};

/// ADX threshold for Chop detection
struct ChopThresholds {
    double adx_max{18.0};
};

/// Hysteresis and transition policy
struct TransitionPolicy {
    int confirmation_ticks{3};             ///< Ticks a new regime must persist before switching
    double min_confidence_to_switch{0.55}; ///< Min confidence to accept a regime change
    double inertia_alpha{0.15};            ///< EMA smoothing for regime transition [0=max inertia, 1=instant]
    int dwell_time_ticks{5};               ///< Min ticks in a regime before allowing another switch
};

/// Confidence/stability calculation policy
struct ConfidencePolicy {
    double base_confidence{0.5};
    double data_quality_weight{0.2};       ///< Weight of valid-indicator count [0,1]
    int max_indicator_count{6};            ///< Denominator for data quality fraction
    double strong_trend_bonus_scale{0.2};
    double anomaly_confidence{0.9};
    double weak_regime_penalty{0.1};
    double same_regime_stability{0.9};
    double first_classification_stability{0.5};
    double intra_group_stability{0.6};
    double inter_group_stability{0.3};
};

/// Per-regime policy defaults
[[nodiscard]] inline RegimePolicy default_policy(DetailedRegime regime) {
    switch (regime) {
        case DetailedRegime::StrongUptrend:
        case DetailedRegime::StrongDowntrend:
            return {StressAction::Allow, 1.0, 0.90, true, true};

        case DetailedRegime::WeakUptrend:
        case DetailedRegime::WeakDowntrend:
            return {StressAction::Allow, 0.85, 1.0, true, true};

        case DetailedRegime::MeanReversion:
            return {StressAction::Allow, 0.75, 1.10, true, true};

        case DetailedRegime::VolatilityExpansion:
            return {StressAction::ReduceSize, 0.55, 0.95, true, true};

        case DetailedRegime::LowVolCompression:
            return {StressAction::Allow, 0.65, 1.05, true, true};

        case DetailedRegime::Chop:
            return {StressAction::ReduceSize, 0.35, 1.35, true, true};

        case DetailedRegime::LiquidityStress:
            return {StressAction::BlockEntry, 0.20, 1.25, false, true};

        case DetailedRegime::SpreadInstability:
            return {StressAction::RaiseThreshold, 0.25, 1.20, false, true};

        case DetailedRegime::ToxicFlow:
            return {StressAction::BlockEntry, 0.10, 1.40, false, true};

        case DetailedRegime::AnomalyEvent:
            return {StressAction::HaltAll, 0.0, 1.50, false, true};

        case DetailedRegime::Undefined:
        default:
            return {StressAction::ReduceSize, 0.50, 1.0, true, true};
    }
}

/// Complete regime engine configuration
struct RegimeConfig {
    TrendThresholds trend;
    MeanReversionThresholds mean_reversion;
    VolatilityThresholds volatility;
    StressThresholds stress;
    ChopThresholds chop;
    TransitionPolicy transition;
    ConfidencePolicy confidence;

    /// Per-regime downstream policies (auto-populated with defaults if empty)
    std::unordered_map<int, RegimePolicy> regime_policies;

    /// Get policy for a specific regime
    [[nodiscard]] RegimePolicy get_policy(DetailedRegime r) const {
        auto it = regime_policies.find(static_cast<int>(r));
        if (it != regime_policies.end()) return it->second;
        return default_policy(r);
    }
};

/// Build default RegimeConfig with production-safe values
[[nodiscard]] inline RegimeConfig make_default_regime_config() {
    return RegimeConfig{};
}

} // namespace tb::regime
