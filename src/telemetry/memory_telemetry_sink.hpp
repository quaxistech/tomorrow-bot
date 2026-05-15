#pragma once
/**
 * @file memory_telemetry_sink.hpp
 * @brief In-memory приёмник телеметрии — для тестов
 */
#include "telemetry/telemetry_sink.hpp"
#include <vector>
#include <mutex>

namespace tb::telemetry {

/// Приёмник телеметрии в оперативной памяти (без файлового ввода-вывода)
class MemoryTelemetrySink final : public ITelemetrySink {
public:
    MemoryTelemetrySink() = default;
    ~MemoryTelemetrySink() override = default;

    /// Принять конверт телеметрии
    VoidResult emit(const TelemetryEnvelope& envelope) override;

    /// Сброс буферов (noop для in-memory)
    VoidResult flush() override;

    /// Получить все захваченные конверты (для тестов)
    [[nodiscard]] std::vector<TelemetryEnvelope> get_envelopes() const;

    /// Количество сохранённых конвертов
    [[nodiscard]] size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<TelemetryEnvelope> envelopes_;
};

} // namespace tb::telemetry
