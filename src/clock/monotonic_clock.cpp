/**
 * @file monotonic_clock.cpp
 * @brief Реализация монотонных часов
 */
#include "monotonic_clock.hpp"
#include <chrono>

namespace tb::clock {

Timestamp MonotonicClock::now() const {
    auto tp = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
    return Timestamp{ns};
}

std::shared_ptr<IClock> create_monotonic_clock() {
    return std::make_shared<MonotonicClock>();
}

} // namespace tb::clock
