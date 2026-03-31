#pragma once
#include <cstddef>

namespace tb::strategy {

/// Configuration for Breakout strategy (BB compression->expansion)
struct BreakoutConfig {
    std::size_t bandwidth_history_size{10};  ///< BB bandwidth buffer size
    double compression_threshold{0.025};     ///< Lower compression threshold (more signals)
    double expansion_ratio{1.3};             ///< Lower expansion ratio (trigger earlier)
    double adx_min{12.0};                    ///< Lower ADX for more signals
    double adx_strong{25.0};                 ///< Lower strong ADX threshold
    double obv_block_threshold{0.0};         ///< No OBV blocking
    double obv_surge_threshold{0.3};         ///< Lower OBV surge threshold
    double base_conviction{0.42};            ///< Higher base conviction
    double max_conviction{0.98};             ///< Higher cap
    double momentum_bonus{0.18};             ///< Higher momentum bonus
    double adx_bonus{0.12};                  ///< Higher ADX bonus
    double volume_bonus{0.12};               ///< Higher volume bonus
    bool allow_counter_trend{true};          ///< Allow counter-trend breakouts
    double counter_trend_conviction_mult{0.7}; ///< Higher counter-trend multiplier
};

/// Configuration for Momentum strategy (EMA crossover + ADX + acceleration)
/// Scientific basis: dual EMA crossover + ADX confirmation (Wilder)
struct MomentumConfig {
    double adx_min{18.0};                    ///< ADX > 18 = emerging trend (Wilder)
    double momentum_threshold{0.002};        ///< Min |momentum_5| (0.2% - more sensitive)
    double rsi_overbought{80.0};             ///< RSI upper guard (wider band)
    double rsi_oversold{20.0};               ///< RSI lower guard (wider band)
    double accel_threshold{0.0005};          ///< Lower accel threshold
    double obv_confirm_threshold{0.15};      ///< Lower OBV confirm
    double base_conviction{0.48};            ///< Above 0.45 threshold + bonuses push higher
    double max_conviction{0.98};             ///< Higher cap
    double adx_weight{0.20};                 ///< ADX weight in conviction
    double momentum_weight{0.25};            ///< Momentum weight (more important)
    double accel_max_bonus{0.20};            ///< Higher accel bonus
    double obv_bonus{0.08};                  ///< Higher OBV bonus
};

/// Configuration for Mean Reversion strategy (BB + RSI extremes)
/// Scientific basis: Ornstein-Uhlenbeck — price reverts to mean at BB extremes
/// Mean reversion IS counter-trend by definition — no counter-trend penalty
struct MeanReversionConfig {
    double bb_buy_threshold{0.30};           ///< BUY at lower 30% of BB (oversold zone)
    double bb_sell_threshold{0.70};          ///< SELL at upper 30% of BB (overbought zone)
    double rsi_buy_threshold{43.0};          ///< BUY when RSI < 43 (1m RSI rarely extreme)
    double rsi_sell_threshold{57.0};         ///< SELL when RSI > 57
    double rsi_panic_low{12.0};              ///< RSI < X = panic (block BUY)
    double rsi_euphoria_high{88.0};          ///< RSI > X = euphoria (block SELL)
    double adx_block_threshold{40.0};        ///< ADX > 40 = very strong trend (Wilder)
    double adx_strong_trend{30.0};           ///< ADX > 30 = strong trend filter (Wilder)
    double macd_bonus_mult{1.25};            ///< MACD confirm multiplier
    double base_conviction{0.50};            ///< Must clear threshold (0.45)
    double max_conviction{0.98};             ///< Higher cap
    double rsi_weight{0.25};                 ///< RSI weight
    double bb_weight{0.25};                  ///< BB weight
    double counter_trend_conv_mult{1.00};    ///< NO penalty — mean reversion IS counter-trend
};

/// Configuration for Microstructure Scalp
/// Scientific basis: Order flow imbalance predicts short-term price movement
/// ADX > 20 (Wilder) = enough trend to ride the flow
struct MicrostructureScalpConfig {
    double imbalance_threshold{0.15};        ///< Min order book imbalance
    double buy_sell_ratio_buy{1.15};         ///< Min buy/sell ratio for BUY
    double buy_sell_ratio_sell{0.87};        ///< Max buy/sell ratio for SELL
    double max_spread_bps{20.0};             ///< Max spread for scalping
    double rsi_upper_guard{72.0};            ///< Block BUY when RSI > 72 (overbought)
    double rsi_lower_guard{28.0};            ///< Block SELL when RSI < 28 (oversold)
    double base_conviction{0.50};            ///< Must clear threshold
    double max_conviction{0.95};             ///< Cap
    double trend_bonus{0.15};                ///< Trend confirmation bonus
    double strong_seller_bonus{0.08};        ///< Dominance bonus
    double limit_price_spread_frac{0.25};    ///< Limit price fraction
    bool block_counter_trend{true};          ///< Block counter-trend entries
    double adx_min{20.0};                    ///< ADX > 20 = some trend (Wilder standard)
    double bb_max_buy{0.90};                 ///< Block BUY when bb_pos > 0.90
    double bb_min_sell{0.10};                ///< Block SELL when bb_pos < 0.10
};

/// Configuration for Vol Expansion (ATR compression->expansion)
struct VolExpansionConfig {
    std::size_t atr_history_size{10};        ///< ATR buffer size
    double compression_ratio{0.75};          ///< Slightly more sensitive
    double expansion_rate_min{0.20};         ///< Lower expansion threshold
    double adx_min{12.0};                    ///< Lower ADX for more signals
    double momentum_threshold{0.002};        ///< Lower momentum threshold
    double base_conviction{0.48};            ///< Must clear 0.45 threshold
    double max_conviction{0.98};             ///< Higher cap
    double expansion_weight{0.25};           ///< Expansion weight
    double adx_weight{0.25};                 ///< ADX weight
    double momentum_weight{0.20};            ///< Higher momentum weight
};

/// Configuration for EMA Pullback (trend + pullback)
/// Scientific basis: ADX > 25 = established trend (Wilder original definition)
struct EmaPullbackConfig {
    double adx_min{25.0};                    ///< ADX > 25 = established trend (Wilder)
    double pullback_depth_min{0.2};          ///< Shallower pullbacks accepted
    double pullback_depth_max{0.85};         ///< Deeper pullbacks OK
    double rsi_buy_min{30.0};                ///< Wider RSI range
    double rsi_buy_max{60.0};                ///< Wider RSI range
    double rsi_sell_max{70.0};               ///< Wider range
    double rsi_sell_min{30.0};               ///< Don't short when RSI < 30 (bounce risk)
    double momentum_recovery_threshold{0.0}; ///< Any recovery counts
    double base_conviction{0.48};            ///< Must clear 0.45 threshold
    double max_conviction{0.95};             ///< Higher cap
    double ema_proximity_weight{0.30};       ///< EMA proximity weight
    double rsi_weight{0.25};                 ///< RSI weight
    double adx_weight{0.20};                 ///< ADX weight
};

/// Configuration for RSI Divergence
struct RsiDivergenceConfig {
    std::size_t lookback_bars{20};           ///< Divergence lookback
    double price_new_extreme_pct{0.0005};    ///< More sensitive extreme detection
    double rsi_divergence_min{1.5};          ///< Lower divergence threshold
    double rsi_oversold{38.0};               ///< Wider oversold zone
    double rsi_overbought{62.0};             ///< Wider overbought zone
    double adx_max{40.0};                    ///< Allow stronger trends
    double macd_confirm_weight{0.18};        ///< Higher MACD bonus
    double base_conviction{0.48};            ///< Must clear 0.45 threshold
    double max_conviction{0.92};             ///< Higher cap
    double divergence_strength_weight{0.30}; ///< Divergence weight
    double rsi_depth_weight{0.25};           ///< RSI depth weight
};

/// Configuration for VWAP Reversion
struct VwapReversionConfig {
    double deviation_entry_pct{0.002};       ///< Lower entry threshold (0.2%)
    double deviation_max_pct{0.020};         ///< Allow wider deviations
    double rsi_buy_max{50.0};                ///< Wider RSI band
    double rsi_sell_min{50.0};               ///< Wider RSI band
    double adx_max{35.0};                    ///< Allow stronger trends
    double volume_confirm_threshold{0.15};   ///< Lower volume confirm
    double base_conviction{0.48};            ///< Must clear 0.45 threshold
    double max_conviction{0.92};             ///< Higher cap
    double deviation_weight{0.35};           ///< Deviation weight
    double rsi_weight{0.25};                 ///< RSI weight
    double volume_weight{0.15};              ///< Volume weight
};

/// Configuration for Volume Profile Reversion (POC/VA levels)
struct VolumeProfileConfig {
    double poc_proximity_pct{0.003};         ///< Lower proximity threshold
    double va_proximity_pct{0.002};          ///< Lower VA threshold
    double rsi_confirm_buy{50.0};            ///< Wider RSI range
    double rsi_confirm_sell{50.0};           ///< Wider RSI range
    double adx_max{40.0};                    ///< Allow stronger trends
    double base_conviction{0.48};            ///< Must clear 0.45 threshold
    double max_conviction{0.92};             ///< Higher cap
    double poc_weight{0.35};                 ///< POC weight
    double va_weight{0.25};                  ///< VA weight
    double rsi_weight{0.20};                 ///< RSI weight
};

} // namespace tb::strategy
