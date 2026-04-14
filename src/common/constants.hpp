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
} // namespace finance

/// Комиссии биржи Bitget (USDT-M Futures)
namespace fees {
    constexpr double kDefaultTakerFeePct = 0.0006;  ///< Futures Taker-комиссия 0.06%
    constexpr double kDefaultMakerFeePct = 0.0002;  ///< Futures Maker-комиссия 0.02%
} // namespace fees

/// Лимиты биржи Bitget (USDT-M futures)
namespace exchange_limits {
    /// Абсолютный минимальный нотионал ордера на Bitget USDT-M futures (USDT).
    /// Реальный минимум зависит от инструмента (от $1 для альткоинов до $5 для BTC/ETH).
    /// Значение $1.10 — безопасный пол для любого инструмента с 10% запасом.
    /// Фактический per-symbol минимум приходит через ExchangeSymbolRules::min_trade_usdt.
    constexpr double kMinBitgetNotionalUsdt = 1.10;

    /// Минимальный нотионал для определения пылевой позиции (ниже — игнорируется)
    constexpr double kDustNotionalUsdt = 0.50;
} // namespace exchange_limits

} // namespace tb::common
