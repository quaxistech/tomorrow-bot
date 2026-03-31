/**
 * @file circuit_breaker.cpp
 * @brief Реализация circuit breaker для защиты от каскадных отказов
 */

#include "resilience/circuit_breaker.hpp"

#include <chrono>

namespace tb::resilience {

// ============================================================
// Конструктор
// ============================================================

CircuitBreaker::CircuitBreaker(std::string name, CircuitBreakerConfig config)
    : name_(std::move(name))
    , config_(config)
{
}

// ============================================================
// allow_request — проверка разрешения на выполнение запроса
// ============================================================

bool CircuitBreaker::allow_request() const {
    const auto current = static_cast<CircuitState>(state_.load(std::memory_order_acquire));

    switch (current) {
        case CircuitState::Closed:
            return true;

        case CircuitState::Open: {
            // Проверяем, истёк ли recovery_timeout
            const int64_t elapsed = now_ms() - last_failure_time_ms_.load(std::memory_order_acquire);
            if (elapsed >= config_.recovery_timeout_ms) {
                // Переходим в HalfOpen
                state_.store(static_cast<int>(CircuitState::HalfOpen), std::memory_order_release);
                half_open_attempts_.store(0, std::memory_order_release);
                return true;
            }
            return false;
        }

        case CircuitState::HalfOpen: {
            // Разрешаем ограниченное число попыток
            const int attempts = half_open_attempts_.fetch_add(1, std::memory_order_acq_rel);
            return attempts < config_.half_open_max_attempts;
        }
    }

    return false;
}

// ============================================================
// record_success — фиксация успешного результата
// ============================================================

void CircuitBreaker::record_success() {
    const auto current = static_cast<CircuitState>(state_.load(std::memory_order_acquire));

    if (current == CircuitState::HalfOpen) {
        // Восстанавливаем нормальную работу
        state_.store(static_cast<int>(CircuitState::Closed), std::memory_order_release);
    }

    consecutive_failures_.store(0, std::memory_order_release);
    half_open_attempts_.store(0, std::memory_order_release);
}

// ============================================================
// record_failure — фиксация неуспешного результата
// ============================================================

void CircuitBreaker::record_failure() {
    last_failure_time_ms_.store(now_ms(), std::memory_order_release);

    const auto current = static_cast<CircuitState>(state_.load(std::memory_order_acquire));

    if (current == CircuitState::HalfOpen) {
        // Из HalfOpen сразу в Open — инкремент consecutive_failures_ для корректного счётчика
        consecutive_failures_.fetch_add(1, std::memory_order_acq_rel);
        state_.store(static_cast<int>(CircuitState::Open), std::memory_order_release);
        return;
    }

    const int failures = consecutive_failures_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (failures >= config_.failure_threshold) {
        state_.store(static_cast<int>(CircuitState::Open), std::memory_order_release);
    }
}

// ============================================================
// Геттеры
// ============================================================

CircuitState CircuitBreaker::state() const {
    return static_cast<CircuitState>(state_.load(std::memory_order_acquire));
}

const std::string& CircuitBreaker::name() const {
    return name_;
}

int CircuitBreaker::consecutive_failures() const {
    return consecutive_failures_.load(std::memory_order_acquire);
}

// ============================================================
// reset — сброс в начальное состояние
// ============================================================

void CircuitBreaker::reset() {
    state_.store(static_cast<int>(CircuitState::Closed), std::memory_order_release);
    consecutive_failures_.store(0, std::memory_order_release);
    half_open_attempts_.store(0, std::memory_order_release);
    last_failure_time_ms_.store(0, std::memory_order_release);
}

// ============================================================
// now_ms — текущее время (steady_clock)
// ============================================================

int64_t CircuitBreaker::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace tb::resilience
