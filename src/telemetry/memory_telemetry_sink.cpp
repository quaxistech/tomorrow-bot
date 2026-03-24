/**
 * @file memory_telemetry_sink.cpp
 * @brief Реализация in-memory приёмника телеметрии
 */
#include "telemetry/memory_telemetry_sink.hpp"

namespace tb::telemetry {

VoidResult MemoryTelemetrySink::emit(const TelemetryEnvelope& envelope) {
    std::lock_guard lock(mutex_);
    envelopes_.push_back(envelope);
    return OkVoid();
}

VoidResult MemoryTelemetrySink::flush() {
    // In-memory — сброс не требуется
    return OkVoid();
}

std::vector<TelemetryEnvelope> MemoryTelemetrySink::get_envelopes() const {
    std::lock_guard lock(mutex_);
    return envelopes_;
}

size_t MemoryTelemetrySink::size() const {
    std::lock_guard lock(mutex_);
    return envelopes_.size();
}

} // namespace tb::telemetry
