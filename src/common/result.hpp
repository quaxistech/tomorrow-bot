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
#include <functional>

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

// ============================================================
// Макрос для раннего возврата при ошибке (аналог ? в Rust)
// ============================================================

/**
 * @brief Возвращает ошибку из функции, если expr завершился с ошибкой
 * 
 * Использование:
 *   TB_TRY(some_result);           // возвращает ошибку если есть
 *   auto val = TB_TRY_VAL(some_result); // извлекает значение или возвращает ошибку
 */
#define TB_TRY(expr)                                    \
    do {                                                \
        auto&& _tb_result = (expr);                     \
        if (!_tb_result) {                              \
            return ::tb::Err<decltype(auto)>(_tb_result.error()); \
        }                                               \
    } while(0)

#define TB_TRY_VOID(expr)                               \
    do {                                                \
        auto&& _tb_result = (expr);                     \
        if (!_tb_result) {                              \
            return ::std::unexpected(_tb_result.error()); \
        }                                               \
    } while(0)

#define TB_TRY_VAL(var, expr)                           \
    auto var = [&]() -> decltype(expr) { return (expr); }(); \
    if (!var) { return ::std::unexpected(var.error()); }

} // namespace tb
