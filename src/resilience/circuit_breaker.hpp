/**
 * @file circuit_breaker.hpp
 * @brief Circuit breaker для защиты от каскадных отказов
 *
 * Реализует паттерн Circuit Breaker (Closed → Open → HalfOpen → Closed).
 * Потокобезопасен через std::atomic.
 */
#pragma once

#include "resilience/resilience_types.hpp"

#include <atomic>
#include <string>

namespace tb::resilience {

/// @brief Circuit breaker для защиты от каскадных отказов
class CircuitBreaker {
public:
    explicit CircuitBreaker(std::string name, CircuitBreakerConfig config = {});

    /// @brief Можно ли выполнить запрос (false = circuit open)
    [[nodiscard]] bool allow_request() const;

    /// @brief Записать успешный результат
    void record_success();

    /// @brief Записать неуспешный результат
    void record_failure();

    /// @brief Текущее состояние
    [[nodiscard]] CircuitState state() const;

    /// @brief Имя (для метрик/логов)
    [[nodiscard]] const std::string& name() const;

    /// @brief Счётчик последовательных отказов
    [[nodiscard]] int consecutive_failures() const;

    /// @brief Сброс в начальное состояние
    void reset();

private:
    /// @brief Получить текущее время в миллисекундах (steady_clock)
    [[nodiscard]] static int64_t now_ms() noexcept;

    std::string name_;
    CircuitBreakerConfig config_;
    mutable std::atomic<int> state_{static_cast<int>(CircuitState::Closed)};
    std::atomic<int> consecutive_failures_{0};
    mutable std::atomic<int> half_open_attempts_{0};
    std::atomic<int64_t> last_failure_time_ms_{0};
};

} // namespace tb::resilience
