/**
 * @file circuit_breaker.hpp
 * @brief Circuit breaker для защиты от каскадных отказов
 *
 * Реализует паттерн Circuit Breaker (Closed → Open → HalfOpen → Closed).
 * Потокобезопасен через std::atomic с CAS для linearizable transitions.
 * Поддерживает injected clock для детерминированного тестирования
 * и опциональные метрики для observability.
 */
#pragma once

#include "resilience/resilience_types.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace tb::resilience {

/// @brief Circuit breaker для защиты от каскадных отказов
class CircuitBreaker {
public:
    explicit CircuitBreaker(std::string name, CircuitBreakerConfig config = {},
                            std::shared_ptr<clock::IClock> clock = nullptr,
                            std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

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
    /// @brief Получить текущее время в миллисекундах
    [[nodiscard]] int64_t now_ms() const noexcept;

    /// @brief Метрика: переход состояния
    void emit_transition(CircuitState from, CircuitState to) const;

    std::string name_;
    CircuitBreakerConfig config_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    mutable std::atomic<int> state_{static_cast<int>(CircuitState::Closed)};
    std::atomic<int> consecutive_failures_{0};
    mutable std::atomic<int> half_open_attempts_{0};
    std::atomic<int64_t> last_failure_time_ms_{0};
};

} // namespace tb::resilience
