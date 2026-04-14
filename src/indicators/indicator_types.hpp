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

} // namespace tb::indicators
