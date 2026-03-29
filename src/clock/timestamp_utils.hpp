/**
 * @file timestamp_utils.hpp
 * @brief Вспомогательные функции для работы с временны́ми метками
 * 
 * Конвертации между наносекундами, миллисекундами и ISO-8601 строками.
 */
#pragma once

#include "common/types.hpp"
#include <string>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace tb::clock {

/// Получить текущее время в наносекундах (inline для максимальной производительности)
[[nodiscard]] inline Timestamp now_ns() noexcept {
    auto tp = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
    return Timestamp{ns};
}

/// Конвертировать временну́ю метку в ISO-8601 строку
[[nodiscard]] inline std::string timestamp_to_iso8601(Timestamp ts) {
    int64_t ns  = ts.get();
    int64_t sec = ns / 1'000'000'000LL;
    int64_t ms  = (ns % 1'000'000'000LL) / 1'000'000LL;

    std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setw(3) << std::setfill('0') << ms << "Z";
    return oss.str();
}

/// Конвертировать временну́ю метку в миллисекунды
[[nodiscard]] inline int64_t timestamp_to_ms(Timestamp ts) noexcept {
    return ts.get() / 1'000'000LL;
}

/// Конвертировать миллисекунды в временну́ю метку
[[nodiscard]] inline Timestamp ms_to_timestamp(int64_t ms) noexcept {
    return Timestamp{ms * 1'000'000LL};
}

/// Вычислить разницу в микросекундах между двумя метками
[[nodiscard]] inline int64_t elapsed_us(Timestamp start, Timestamp end) noexcept {
    return (end.get() - start.get()) / 1000LL;
}

/// Получить текущее время steady_clock в наносекундах (монотонные часы).
/// Используется для вычисления интервалов и staleness — не зависит от коррекции системных часов.
[[nodiscard]] inline int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace tb::clock
