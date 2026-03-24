/**
 * @file clock.hpp
 * @brief Интерфейс часов системы
 * 
 * Абстракция часов позволяет подменять реализацию в тестах
 * (MockClock с управляемым временем).
 */
#pragma once

#include "common/types.hpp"

namespace tb::clock {

/**
 * @brief Абстрактный интерфейс часов
 * 
 * Возвращает текущее время в наносекундах от Unix-эпохи.
 * Реализации: WallClock (system_clock), MonotonicClock (steady_clock)
 */
class IClock {
public:
    virtual ~IClock() = default;

    /**
     * @brief Получить текущее время
     * @return Временна́я метка в наносекундах от Unix-эпохи
     */
    [[nodiscard]] virtual Timestamp now() const = 0;
};

} // namespace tb::clock
