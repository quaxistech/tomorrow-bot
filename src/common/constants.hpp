#pragma once

#include <cstdint>

namespace tb::common {

/// Константы времени
namespace time {
    constexpr int64_t kOneSecondNs = 1'000'000'000LL;   ///< 1 секунда в наносекундах
    constexpr int64_t kOneMinuteNs = 60'000'000'000LL;  ///< 1 минута в наносекундах
} // namespace time

/// Константы финансовых расчётов
namespace finance {
    constexpr double kPercentScaler = 100.0;       ///< Множитель для перевода в проценты
    constexpr int kBasisPointsScaler = 10000;      ///< Множитель для базисных пунктов (bps)
    constexpr double kFloatEpsilon = 1e-9;         ///< Эпсилон для сравнения чисел с плавающей точкой
    constexpr double kMinValidPrice = 1e-10;       ///< Минимальная валидная цена
} // namespace finance

} // namespace tb::common
