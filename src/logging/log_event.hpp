/**
 * @file log_event.hpp
 * @brief Структура события логирования
 * 
 * LogEvent — базовая единица логирования в системе.
 * Содержит уровень, метку времени, компонент, сообщение
 * и произвольные структурированные поля.
 */
#pragma once

#include "common/types.hpp"
#include <string>
#include <unordered_map>

namespace tb::logging {

/// Уровень важности сообщения лога
enum class LogLevel {
    Trace,      ///< Детальная трассировка (только разработка)
    Debug,      ///< Отладочная информация
    Info,       ///< Информационные сообщения
    Warn,       ///< Предупреждения
    Error,      ///< Ошибки (требуют внимания)
    Critical    ///< Критические ошибки (система может не работать)
};

/// Числовое значение уровня (для сравнения)
[[nodiscard]] constexpr int log_level_value(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:    return 0;
        case LogLevel::Debug:    return 1;
        case LogLevel::Info:     return 2;
        case LogLevel::Warn:     return 3;
        case LogLevel::Error:    return 4;
        case LogLevel::Critical: return 5;
    }
    return 0;
}

/// Строковое представление уровня лога
[[nodiscard]] constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warn:     return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

/**
 * @brief Событие логирования
 * 
 * Передаётся через систему логирования от источника к обработчику.
 * Immutable после создания (копируется при передаче).
 */
struct LogEvent {
    LogLevel    level;                                          ///< Уровень важности
    Timestamp   timestamp;                                      ///< Время события (нс от эпохи)
    std::string component;                                      ///< Имя компонента-источника
    std::string message;                                        ///< Текст сообщения
    std::string correlation_id;                                 ///< ID корреляции для трассировки
    std::unordered_map<std::string, std::string> fields;        ///< Дополнительные структурированные поля
};

} // namespace tb::logging
