#pragma once
/**
 * @file research_telemetry.hpp
 * @brief Исследовательская телеметрия — сбор данных для анализа
 *
 * Захватывает детальные конверты телеметрии и направляет их
 * в подключаемый приёмник (sink) для хранения/вывода.
 */
#include "telemetry/telemetry_types.hpp"
#include "telemetry/telemetry_sink.hpp"
#include "common/result.hpp"
#include <memory>
#include <mutex>
#include <atomic>

namespace tb::telemetry {

/// Исследовательский сборщик телеметрии
class ResearchTelemetry {
public:
    /// Конструктор принимает приёмник и конфигурацию
    explicit ResearchTelemetry(
        std::shared_ptr<ITelemetrySink> sink,
        TelemetryConfig config = {});

    /// Захватить конверт телеметрии — отправить в приёмник
    VoidResult capture(const TelemetryEnvelope& envelope);

    /// Сбросить буферы приёмника
    VoidResult flush();

    /// Количество захваченных конвертов (атомарный счётчик)
    [[nodiscard]] uint64_t get_captured_count() const;

    /// Включена ли телеметрия
    [[nodiscard]] bool is_enabled() const;

private:
    std::shared_ptr<ITelemetrySink> sink_;
    TelemetryConfig config_;
    std::atomic<uint64_t> captured_count_{0};
    std::mutex mutex_;
};

} // namespace tb::telemetry
