/**
 * @file wall_clock.cpp
 * @brief Реализация системных часов
 */
#include "wall_clock.hpp"
#include <chrono>

namespace tb::clock {

Timestamp WallClock::now() const {
    auto tp = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
    return Timestamp{ns};
}

std::shared_ptr<IClock> create_wall_clock() {
    return std::make_shared<WallClock>();
}

} // namespace tb::clock
