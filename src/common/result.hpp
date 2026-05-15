/**
 * @file result.hpp
 * @brief Тип результата на основе std::expected<T, TbError>
 * 
 * Обеспечивает удобные алиасы и вспомогательные функции для работы
 * с результатами операций без исключений.
 * 
 * Паттерн: монадическая обработка ошибок через цепочки операций.
 */
#pragma once

#include "errors.hpp"
#include <expected>

namespace tb {

// ============================================================
// Алиас типа результата
// ============================================================

/**
 * @brief Основной тип результата системы
 * @tparam T Тип успешного значения
 * 
 * Пример использования:
 *   Result<AppConfig> load_config(std::string_view path);
 *   auto result = load_config("paper.yaml");
 *   if (result) { use(*result); }
 *   else { handle(result.error()); }
 */
template<typename T>
using Result = std::expected<T, TbError>;

/// Специализация для операций без возвращаемого значения
using VoidResult = std::expected<void, TbError>;

// ============================================================
// Вспомогательные функции создания результатов
// ============================================================

/// Создаёт успешный результат
template<typename T>
[[nodiscard]] inline Result<T> Ok(T&& value) {
    return std::expected<T, TbError>{std::forward<T>(value)};
}

/// Создаёт результат с ошибкой
template<typename T = void>
[[nodiscard]] inline Result<T> Err(TbError error) {
    return std::unexpected<TbError>{error};
}

/// Успешный пустой результат
[[nodiscard]] inline VoidResult OkVoid() {
    return {};
}

/// Пустой результат с ошибкой
[[nodiscard]] inline VoidResult ErrVoid(TbError error) {
    return std::unexpected<TbError>{error};
}

} // namespace tb
