/**
 * @file monotonic_clock.hpp
 * @brief Монотонные часы для измерения интервалов
 * 
 * Использует std::chrono::steady_clock — монотонно возрастающий счётчик.
 * Подходит для измерения задержек и интервалов.
 * НЕ подходит для астрономического времени (значение не UTC).
 */
#pragma once

#include "clock.hpp"
#include <memory>

namespace tb::clock {

/**
 * @brief Монотонные часы
 * 
 * Гарантируют монотонное возрастание — не подвержены прыжкам NTP.
 * Используются для измерения задержки исполнения ордеров.
 */
class MonotonicClock : public IClock {
public:
    [[nodiscard]] Timestamp now() const override;
};

/// Создаёт экземпляр монотонных часов
[[nodiscard]] std::shared_ptr<IClock> create_monotonic_clock();

} // namespace tb::clock
