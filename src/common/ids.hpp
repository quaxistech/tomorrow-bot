/**
 * @file ids.hpp
 * @brief Утилиты генерации уникальных идентификаторов
 * 
 * Генерация UUID-подобных идентификаторов для ордеров, сделок
 * и корреляционных идентификаторов трассировки.
 * 
 * Реализация: UUID v4 на основе /dev/urandom или std::random_device
 */
#pragma once

#include "types.hpp"
#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <array>

namespace tb {

// ============================================================
// Генератор UUID v4
// ============================================================

/**
 * @brief Генерирует UUID v4 (случайный) в формате xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 */
[[nodiscard]] inline std::string generate_uuid_v4() {
    // Используем thread_local для потокобезопасности без мьютексов
    thread_local std::random_device                 rd;
    thread_local std::mt19937_64                    gen(rd());
    thread_local std::uniform_int_distribution<int> hex_dist(0, 15);
    thread_local std::uniform_int_distribution<int> var_dist(8, 11);

    std::ostringstream oss;
    oss << std::hex;

    // Генерация 32 hex-символов с разделителями
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            oss << '-';
        }
        if (i == 12) {
            oss << '4'; // Версия 4
        } else if (i == 16) {
            oss << var_dist(gen); // Вариант 10xx
        } else {
            oss << hex_dist(gen);
        }
    }

    return oss.str();
}

// ============================================================
// Фабричные функции для доменных идентификаторов
// ============================================================

/// Генерирует новый идентификатор ордера
[[nodiscard]] inline OrderId generate_order_id() {
    return OrderId{"ord-" + generate_uuid_v4()};
}

/// Генерирует новый идентификатор сделки
[[nodiscard]] inline TradeId generate_trade_id() {
    return TradeId{"trd-" + generate_uuid_v4()};
}

/// Генерирует новый корреляционный идентификатор для трассировки
[[nodiscard]] inline CorrelationId generate_correlation_id() {
    return CorrelationId{"cor-" + generate_uuid_v4()};
}

/// Генерирует короткий идентификатор (первые 8 символов UUID)
[[nodiscard]] inline std::string generate_short_id() {
    return generate_uuid_v4().substr(0, 8);
}

} // namespace tb
