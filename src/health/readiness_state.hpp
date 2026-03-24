/**
 * @file readiness_state.hpp
 * @brief Состояния готовности системы и подсистем
 * 
 * Используется для health-check эндпоинтов (liveness/readiness probes).
 * Совместим с Kubernetes health check протоколом.
 */
#pragma once

#include "common/types.hpp"
#include <string>

namespace tb::health {

/// Общая готовность системы к приёму трафика
enum class ReadinessState {
    NotReady,   ///< Система не готова (инициализация, критическая ошибка)
    Degraded,   ///< Система работает с ограничениями
    Ready       ///< Система полностью готова к работе
};

/// Состояние отдельной подсистемы
enum class SubsystemState {
    Unknown,    ///< Состояние неизвестно (начальное)
    Starting,   ///< Подсистема запускается
    Healthy,    ///< Подсистема работает нормально
    Degraded,   ///< Подсистема работает с ограничениями
    Failed      ///< Подсистема вышла из строя
};

/// Преобразование SubsystemState в строку
[[nodiscard]] constexpr std::string_view to_string(SubsystemState state) noexcept {
    switch (state) {
        case SubsystemState::Unknown:  return "unknown";
        case SubsystemState::Starting: return "starting";
        case SubsystemState::Healthy:  return "healthy";
        case SubsystemState::Degraded: return "degraded";
        case SubsystemState::Failed:   return "failed";
    }
    return "unknown";
}

/// Преобразование ReadinessState в строку
[[nodiscard]] constexpr std::string_view to_string(ReadinessState state) noexcept {
    switch (state) {
        case ReadinessState::NotReady: return "not_ready";
        case ReadinessState::Degraded: return "degraded";
        case ReadinessState::Ready:    return "ready";
    }
    return "not_ready";
}

/**
 * @brief Состояние здоровья отдельной подсистемы
 */
struct SubsystemHealth {
    std::string    name;            ///< Имя подсистемы
    SubsystemState state;           ///< Текущее состояние
    std::string    message;         ///< Дополнительная информация
    Timestamp      last_updated;    ///< Время последнего обновления
};

} // namespace tb::health
