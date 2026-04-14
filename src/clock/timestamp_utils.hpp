/**
 * @file timestamp_utils.hpp
 * @brief Монотонное время для измерения интервалов и staleness
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace tb::clock {

/// Получить текущее время steady_clock в наносекундах (монотонные часы).
/// Используется для вычисления интервалов и staleness — не зависит от коррекции системных часов.
[[nodiscard]] inline int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace tb::clock
