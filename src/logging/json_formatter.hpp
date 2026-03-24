/**
 * @file json_formatter.hpp
 * @brief Форматирование событий лога в JSON строку
 * 
 * Используется для структурированного логирования.
 * Формат совместим с системами сбора логов (ELK, Loki и т.д.).
 */
#pragma once

#include "log_event.hpp"
#include <string>

namespace tb::logging {

/**
 * @brief Форматирует LogEvent как строку JSON
 * 
 * Формат вывода:
 * {"timestamp":"...","level":"INFO","component":"...","message":"...","correlation_id":"...","fields":{...}}
 * 
 * @param event Событие для форматирования
 * @return JSON строка (однострочная, без переносов)
 */
[[nodiscard]] std::string format_as_json(const LogEvent& event);

/// Экранирует специальные символы в JSON строке
[[nodiscard]] std::string json_escape(std::string_view s);

/// Форматирует временну́ю метку как ISO-8601 строку
[[nodiscard]] std::string timestamp_to_iso8601(Timestamp ts);

} // namespace tb::logging
