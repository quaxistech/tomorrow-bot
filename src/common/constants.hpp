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

    /// Round-trip taker fee как фракция (entry+exit).
    constexpr double kRoundTripTakerFraction = kDefaultTakerFeePct * 2.0;
    /// Round-trip taker fee в процентах.
    constexpr double kRoundTripTakerPct = kRoundTripTakerFraction * 100.0;
    /// Экономический пол для ATR%: цена должна двигаться хотя бы на этот процент,
    /// чтобы перекрыть round-trip taker fee с разумным запасом.
    constexpr double kEconomicAtrSafetyMultiplier = 1.25;
    constexpr double kEconomicAtrFloorPct = kRoundTripTakerPct * kEconomicAtrSafetyMultiplier;
    constexpr double kEconomicAtrFloorFraction = kRoundTripTakerFraction * kEconomicAtrSafetyMultiplier;
} // namespace fees

/// Лимиты биржи Bitget (USDT-M futures)
namespace exchange_limits {
    /// Абсолютный минимальный нотионал ордера на Bitget USDT-M futures (USDT).
    /// Реальный минимум зависит от инструмента (от $1 для альткоинов до $5 для BTC/ETH).
    /// Значение $1.10 — безопасный пол для любого инструмента с 10% запасом.
    /// Фактический per-symbol минимум приходит через ExchangeSymbolRules::min_trade_usdt.
    // BUG-EDGE-4 (live run14): был 1.10 — Bitget reject 45110 на LABUSDT (real min $5).
    // Real Bitget USDT-M minimum часто $5, безопасное умолчание.
    constexpr double kMinBitgetNotionalUsdt = 5.5;

    /// Минимальный нотионал для определения пылевой позиции (ниже — игнорируется)
    constexpr double kDustNotionalUsdt = 0.50;
} // namespace exchange_limits

} // namespace tb::common
