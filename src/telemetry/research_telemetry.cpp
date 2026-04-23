/**
 * @file research_telemetry.cpp
 * @brief Реализация исследовательского сборщика телеметрии
 */
#include "telemetry/research_telemetry.hpp"

namespace tb::telemetry {

ResearchTelemetry::ResearchTelemetry(
    std::shared_ptr<ITelemetrySink> sink,
    TelemetryConfig config)
    : sink_(std::move(sink))
    , config_(std::move(config)) {}

VoidResult ResearchTelemetry::capture(const TelemetryEnvelope& envelope) {
    if (!config_.enabled) {
        return OkVoid();
    }

    std::lock_guard lock(mutex_);
    auto result = sink_->emit(envelope);
    if (result) {
        captured_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

VoidResult ResearchTelemetry::flush() {
    std::lock_guard lock(mutex_);
    return sink_->flush();
}

uint64_t ResearchTelemetry::get_captured_count() const {
    return captured_count_.load(std::memory_order_relaxed);
}

bool ResearchTelemetry::is_enabled() const {
    return config_.enabled;
}

} // namespace tb::telemetry
