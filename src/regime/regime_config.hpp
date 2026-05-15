#pragma once

#include <string>

#include "regime/regime_types.hpp"

namespace tb::regime {

/// Thresholds for trend classification
/// Научное обоснование порогов ADX: Welles Wilder (1978) "New Concepts in Technical Trading Systems"
/// RSI bias: Constance Brown (1999) "Technical Analysis for the Trading Professional"
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
/// VPIN: Easley, López de Prado, O'Hara (2012) "Flow Toxicity and Liquidity"
/// Book imbalance: Cont, Stoikov, Talreja (2010) "A Stochastic Model for Order Book Dynamics"
struct StressThresholds {
    double rsi_extreme_high{85.0};
    double rsi_extreme_low{15.0};
    double obv_norm_extreme{2.0};
    double aggressive_flow_toxic{0.75};
    double spread_toxic_bps{15.0};        ///< Порог спреда для ToxicFlow [bps]
    double book_instability_threshold{0.6};
    double spread_stress_bps{30.0};
    /// Порог дисбаланса ликвидности: liquidity_ratio = min(bid,ask)/avg(bid,ask) ∈ [0,1].
    /// Значение ниже порога означает сильный перекос книги ордеров (стресс).
    /// 0.3 ≈ соотношение сторон ~3.3:1 (Cont, Stoikov, Talreja 2010).
    double liquidity_ratio_stress{0.3};
};

/// ADX threshold for Chop detection
struct ChopThresholds {
    double adx_max{18.0};
};

/// Hysteresis and transition policy
/// Basseville, Nikiforov (1993) "Detection of Abrupt Changes" — CUSUM-accelerated transitions
struct TransitionPolicy {
    int confirmation_ticks{3};             ///< Ticks a new regime must persist before switching
    double min_confidence_to_switch{0.55}; ///< Min confidence to accept a regime change
    int dwell_time_ticks{5};               ///< Min ticks in a regime before allowing another switch
};

/// Confidence/stability calculation policy
struct ConfidencePolicy {
    double base_confidence{0.5};
    double data_quality_weight{0.2};       ///< Weight of valid-indicator count [0,1]
    /// Количество индикаторов, реально участвующих в классификации:
    /// EMA, RSI, ADX, BB, ATR, OBV, CUSUM, VPIN, trade_flow, spread, instability, liquidity
    int max_indicator_count{12};
    double strong_trend_bonus_scale{0.2};
    double anomaly_confidence{0.9};
    double weak_regime_penalty{0.1};
    double same_regime_stability{0.9};
    double first_classification_stability{0.5};
    double intra_group_stability{0.6};
    double inter_group_stability{0.3};
};

/// Complete regime engine configuration — USDT-M futures scalping
struct RegimeConfig {
    TrendThresholds trend;
    MeanReversionThresholds mean_reversion;
    VolatilityThresholds volatility;
    StressThresholds stress;
    ChopThresholds chop;
    TransitionPolicy transition;
    ConfidencePolicy confidence;
};

/// Build default RegimeConfig with production-safe values
[[nodiscard]] inline RegimeConfig make_default_regime_config() {
    return RegimeConfig{};
}

} // namespace tb::regime
