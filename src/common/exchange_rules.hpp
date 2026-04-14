#pragma once
/**
 * @file exchange_rules.hpp
 * @brief Централизованные ограничения торгового инструмента с биржи
 *
 * Единый объект, описывающий precision, min notional, min qty и step size
 * для конкретного символа. Используется в risk, allocator, execution и submitter.
 */

#include "common/types.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tb {

/// Максимальная точность для snprintf %.*f (защита от UB)
inline constexpr int kMaxPrecisionDigits = 18;

/// Ограничения торгового инструмента USDT-M futures, полученные с биржи (exchange info)
struct ExchangeSymbolRules {
    Symbol symbol{Symbol("")};

    int quantity_precision{6};    ///< Количество десятичных знаков для quantity (base)
    int price_precision{2};       ///< Количество десятичных знаков для price (quote)
    double min_trade_usdt{5.0};   ///< Минимальный notional (USDT) для ордера
    double min_quantity{0.0};     ///< Минимальный quantity (base), 0 = не задан
    double max_quantity{0.0};     ///< Максимальный quantity (base), 0 = не задан

    /// Округлить quantity вниз до допустимой точности (floor).
    /// Использует snprintf для избежания накопленных ошибок floating-point
    /// при высоком значении quantity_precision (>6).
    [[nodiscard]] double floor_quantity(double qty) const noexcept {
        if (qty <= 0.0) return 0.0;
        const int prec = std::clamp(quantity_precision, 0, kMaxPrecisionDigits);
        double factor = 1.0;
        for (int i = 0; i < prec; ++i) factor *= 10.0;
        double floored = std::floor(qty * factor);
        // snprintf → strtod roundtrip для точного представления
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", prec, floored / factor);
        return std::strtod(buf, nullptr);
    }

    /// Округлить цену до допустимой точности (round half-up)
    [[nodiscard]] double round_price(double price) const noexcept {
        if (price <= 0.0) return 0.0;
        const int prec = std::clamp(price_precision, 0, kMaxPrecisionDigits);
        double factor = 1.0;
        for (int i = 0; i < prec; ++i) factor *= 10.0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", prec, std::round(price * factor) / factor);
        return std::strtod(buf, nullptr);
    }

    /// Проверить, что quantity допустим (не ноль, не пыль, >= min_quantity)
    [[nodiscard]] bool is_quantity_valid(double qty) const noexcept {
        if (qty <= 0.0) return false;
        if (min_quantity > 0.0 && qty < min_quantity) return false;
        if (max_quantity > 0.0 && qty > max_quantity) return false;
        return true;
    }

    /// Проверить, что notional >= min_trade_usdt
    [[nodiscard]] bool is_notional_valid(double notional) const noexcept {
        return notional >= min_trade_usdt;
    }

    /// Полная валидация ордера перед отправкой
    [[nodiscard]] bool validate_order(double qty, double price) const noexcept {
        double floored_qty = floor_quantity(qty);
        if (!is_quantity_valid(floored_qty)) return false;
        double rounded_price = round_price(price);
        double notional = floored_qty * rounded_price;
        return is_notional_valid(notional);
    }

    /// Форматировать quantity для отправки на биржу
    [[nodiscard]] std::string format_quantity(double qty) const {
        double floored = floor_quantity(qty);
        const int prec = std::clamp(quantity_precision, 0, kMaxPrecisionDigits);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", prec, floored);
        return std::string(buf);
    }

    /// Форматировать цену для отправки на биржу
    [[nodiscard]] std::string format_price(double price) const {
        double rounded = round_price(price);
        const int prec = std::clamp(price_precision, 0, kMaxPrecisionDigits);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", prec, rounded);
        return std::string(buf);
    }
};

} // namespace tb
