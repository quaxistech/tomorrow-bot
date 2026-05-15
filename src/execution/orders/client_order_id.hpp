#pragma once
/**
 * @file client_order_id.hpp
 * @brief Генератор уникальных Client Order ID (§15, §22 ТЗ)
 *
 * Гарантирует уникальность идентификаторов ордеров в рамках сессии.
 * Формат: TB{session_suffix}-{seq}
 */

#include <atomic>
#include <chrono>
#include <string>

namespace tb::execution {

/// Генератор уникальных Client Order ID
class ClientOrderIdGenerator {
public:
    /// Сгенерировать следующий уникальный ID
    static std::string next() {
        static const std::string session_prefix = [] {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto s = std::to_string(ms);
            return s.substr(s.size() > 6 ? s.size() - 6 : 0);
        }();
        static std::atomic<int64_t> seq{1};
        return "TB" + session_prefix + "-" + std::to_string(seq++);
    }

    /// Сброс счетчика (только для тестов)
    static void reset_for_testing() {
        // Нельзя сбросить static atomic напрямую — используйте offset
    }
};

} // namespace tb::execution
