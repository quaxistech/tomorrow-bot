#pragma once
#include <string>

namespace tb::indicators {

enum class IndicatorStatus {
    Ok,
    InsufficientData,
    InvalidInput
};

// ---------------------------------------------------------------------------
// Result structs — new metadata fields are appended after the original fields
// so that existing aggregate-initialization patterns still compile.
// `valid` is kept as a convenience; it should be true when status == Ok.
// ---------------------------------------------------------------------------

struct IndicatorResult {
    bool valid{false};
    double value{0.0};
    std::string name;
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
    int warmup_remaining{0};
};

struct MacdResult {
    bool valid{false};
    double macd{0.0};
    double signal{0.0};
    double histogram{0.0};
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
    int warmup_remaining{0};
};

struct BollingerResult {
    bool valid{false};
    double upper{0.0};
    double middle{0.0};
    double lower{0.0};
    double bandwidth{0.0};
    double percent_b{0.0};
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
    int warmup_remaining{0};
};

struct AdxResult {
    bool valid{false};
    double adx{0.0};
    double plus_di{0.0};
    double minus_di{0.0};
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
    int warmup_remaining{0};
};

struct VwapResult {
    bool valid{false};
    double vwap{0.0};
    double upper_band{0.0};
    double lower_band{0.0};
    double cumulative_volume{0.0};
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
    int warmup_remaining{0};
};

// run93 (2026-05-17): индикаторы для повышения precision сигналов.

/// Supertrend (Olson 2008): ATR-based trend follower.
/// Принцип: цена выше supertrend → uptrend (long bias). Ниже → downtrend.
/// Параметры: ATR period (default 10), multiplier (default 3.0).
struct SupertrendResult {
    bool valid{false};
    double value{0.0};        ///< Текущий уровень supertrend
    int trend{0};             ///< +1 = uptrend, -1 = downtrend, 0 = undefined
    bool flipped{false};      ///< Flag: текущий тик — момент смены тренда
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
};

/// Stochastic Oscillator (Lane 1957): position of current price в recent range.
/// %K = (close - low_N) / (high_N - low_N) × 100
/// %D = SMA(%K, smooth_period)
/// Signals: %K crosses %D + position in extreme zone.
struct StochasticResult {
    bool valid{false};
    double k{0.0};                ///< %K — fast line [0..100]
    double d{0.0};                ///< %D — signal line [0..100]
    bool overbought{false};       ///< %K > 80
    bool oversold{false};         ///< %K < 20
    bool bull_cross{false};       ///< %K just crossed above %D in oversold zone
    bool bear_cross{false};       ///< %K just crossed below %D in overbought zone
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
};

/// Fast EMA pair (9/21) для micro-trend detection.
/// fast EMA пересекает slow EMA → trend change signal.
struct EmaPairResult {
    bool valid{false};
    double ema_fast{0.0};         ///< EMA(9) обычно
    double ema_slow{0.0};         ///< EMA(21) обычно
    int trend{0};                 ///< +1 = fast > slow (bullish), -1 = bearish
    bool bull_cross{false};       ///< fast just crossed above slow
    bool bear_cross{false};       ///< fast just crossed below slow
    double separation_bps{0.0};   ///< |fast-slow|/price × 10000 — сила тренда
    IndicatorStatus status{IndicatorStatus::InsufficientData};
    int sample_count{0};
};

} // namespace tb::indicators
