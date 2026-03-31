#pragma once

#include <cstdint>

namespace tb::common {

/// Константы времени
namespace time {
    constexpr int64_t kOneSecondNs    = 1'000'000'000LL;         ///< 1 секунда в наносекундах
    constexpr int64_t kOneMinuteNs    = 60'000'000'000LL;        ///< 1 минута в наносекундах
    constexpr int64_t kFiveMinutesNs  = 300'000'000'000LL;       ///< 5 минут в наносекундах
    constexpr int64_t kOneHourNs      = 3'600'000'000'000LL;     ///< 1 час в наносекундах
    constexpr int64_t kOneDayNs       = 86'400'000'000'000LL;    ///< 1 день в наносекундах
} // namespace time

/// Константы финансовых расчётов
namespace finance {
    constexpr double kPercentScaler = 100.0;       ///< Множитель для перевода в проценты
    constexpr int kBasisPointsScaler = 10000;      ///< Множитель для базисных пунктов (bps)
    constexpr double kFloatEpsilon = 1e-9;         ///< Эпсилон для сравнения чисел с плавающей точкой
    constexpr double kMinValidPrice = 1e-10;       ///< Минимальная валидная цена
} // namespace finance

/// Комиссии биржи Bitget (USDT-M Futures)
namespace fees {
    constexpr double kDefaultTakerFeePct = 0.0006;  ///< Futures Taker-комиссия 0.06%
    constexpr double kDefaultMakerFeePct = 0.0002;  ///< Futures Maker-комиссия 0.02%
} // namespace fees

/// Лимиты биржи Bitget (spot)
namespace exchange_limits {
    /// Минимальный нотионал ордера на Bitget (USDT).
    /// Реальный минимум биржи = $1.00, используем $1.10 с запасом.
    constexpr double kMinBitgetNotionalUsdt = 1.10;

    /// Минимальный нотионал для определения пылевой позиции (ниже — игнорируется)
    constexpr double kDustNotionalUsdt = 0.50;
} // namespace exchange_limits

} // namespace tb::common
