#pragma once
/**
 * @file telemetry_sink.hpp
 * @brief Интерфейс приёмника телеметрии
 */
#include "telemetry/telemetry_types.hpp"
#include "common/result.hpp"

namespace tb::telemetry {

/// Абстрактный приёмник телеметрических конвертов
class ITelemetrySink {
public:
    virtual ~ITelemetrySink() = default;

    /// Отправить конверт телеметрии в приёмник
    virtual VoidResult emit(const TelemetryEnvelope& envelope) = 0;

    /// Сбросить буферы приёмника
    virtual VoidResult flush() = 0;
};

} // namespace tb::telemetry
