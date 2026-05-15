/**
 * @file wall_clock.hpp
 * @brief Реализация часов на основе системного времени
 * 
 * Использует std::chrono::system_clock — астрономическое время.
 * Подходит для временны́х меток событий и логирования.
 */
#pragma once

#include "clock.hpp"
#include <memory>

namespace tb::clock {

/**
 * @brief Системные часы (wall clock)
 * 
 * Возвращает реальное время системы.
 * Может "прыгать" при синхронизации NTP — не подходит для замера интервалов.
 */
class WallClock : public IClock {
public:
    [[nodiscard]] Timestamp now() const override;
};

/// Создаёт экземпляр системных часов
[[nodiscard]] std::shared_ptr<IClock> create_wall_clock();

} // namespace tb::clock
