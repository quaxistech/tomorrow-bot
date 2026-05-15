/**
 * @file redaction.hpp
 * @brief Утилиты маскирования секретных данных в выводе
 * 
 * Предотвращает случайную утечку API ключей и секретов в логах.
 * RedactedString при выводе в поток печатает "[REDACTED]".
 */
#pragma once

#include <string>
#include <ostream>

namespace tb::security {

/**
 * @brief Обёртка чувствительных строк — маскирует вывод
 * 
 * Используйте для хранения секретов, которые могут попасть в лог.
 * operator<< всегда печатает "[REDACTED]" вместо реального значения.
 */
struct RedactedString {
    std::string value; ///< Реальное значение (доступно напрямую)

    explicit RedactedString(std::string v) : value(std::move(v)) {}
    RedactedString() = default;

    /// Всегда выводит "[REDACTED]" — никогда не раскрывает значение
    friend std::ostream& operator<<(std::ostream& os, const RedactedString&) {
        return os << "[REDACTED]";
    }

    /// Получить реальное значение (только явно)
    [[nodiscard]] const std::string& reveal() const noexcept { return value; }
};

/**
 * @brief Заменяет значения чувствительных полей в JSON строке на [REDACTED]
 * 
 * Обрабатывает поля: api_key, api_secret, apiKey, apiSecret, password, passphrase
 * Используется перед логированием JSON данных.
 * 
 * @param input Исходная JSON строка (передаётся по значению — modifies copy)
 * @return Строка с заменёнными секретами
 */
[[nodiscard]] std::string redact_secrets(std::string input);

} // namespace tb::security
