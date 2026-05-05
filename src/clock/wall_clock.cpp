/**
 * @file wall_clock.cpp
 * @brief Реализация системных часов
 */
#include "wall_clock.hpp"
#include <chrono>

namespace tb::clock {

Timestamp WallClock::now() const {
    // BUG-S33-02: system_clock (CLOCK_REALTIME) can go backward on NTP correction,
    // making elapsed-time calculations negative and disabling safety checks.
    // Use steady_clock (CLOCK_MONOTONIC) — guaranteed monotonic; exchange API
    // timestamps are independently sourced from system_clock in the REST client.
    auto tp = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
    return Timestamp{ns};
}

std::shared_ptr<IClock> create_wall_clock() {
    return std::make_shared<WallClock>();
}

} // namespace tb::clock
