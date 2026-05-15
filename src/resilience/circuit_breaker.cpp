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

CircuitBreaker::CircuitBreaker(std::string name, CircuitBreakerConfig config,
                               std::shared_ptr<clock::IClock> clock,
                               std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : name_(std::move(name))
    , config_(config)
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
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
            // BUG-S34-04: clamp to 0 — NTP backward jump can produce negative elapsed,
            // which would never satisfy >= recovery_timeout_ms, locking CB open forever.
            const int64_t elapsed = std::max(int64_t{0},
                now_ms() - last_failure_time_ms_.load(std::memory_order_acquire));
            if (elapsed >= config_.recovery_timeout_ms) {
                // CAS: только один поток выполняет переход Open → HalfOpen
                int expected = static_cast<int>(CircuitState::Open);
                if (state_.compare_exchange_strong(
                        expected,
                        static_cast<int>(CircuitState::HalfOpen),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    half_open_attempts_.store(1, std::memory_order_release);
                    emit_transition(CircuitState::Open, CircuitState::HalfOpen);
                    return true;
                }
                // Другой поток уже перевёл — пробуем как HalfOpen
                if (expected == static_cast<int>(CircuitState::HalfOpen)) {
                    const int attempts = half_open_attempts_.fetch_add(1, std::memory_order_acq_rel);
                    return attempts < config_.half_open_max_attempts;
                }
                return false;
            }
            return false;
        }

        case CircuitState::HalfOpen: {
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
        state_.store(static_cast<int>(CircuitState::Closed), std::memory_order_release);
        emit_transition(CircuitState::HalfOpen, CircuitState::Closed);
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
        consecutive_failures_.fetch_add(1, std::memory_order_acq_rel);
        state_.store(static_cast<int>(CircuitState::Open), std::memory_order_release);
        emit_transition(CircuitState::HalfOpen, CircuitState::Open);
        return;
    }

    const int failures = consecutive_failures_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (failures >= config_.failure_threshold) {
        state_.store(static_cast<int>(CircuitState::Open), std::memory_order_release);
        emit_transition(CircuitState::Closed, CircuitState::Open);
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
// now_ms — текущее время в миллисекундах
// ============================================================

int64_t CircuitBreaker::now_ms() const noexcept {
    if (clock_) {
        return clock_->now().get() / 1'000'000;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ============================================================
// emit_transition — метрика перехода состояния
// ============================================================

void CircuitBreaker::emit_transition(CircuitState from, CircuitState to) const {
    if (metrics_) {
        metrics_->counter("circuit_breaker_transitions_total",
            {{"name", name_},
             {"from", std::string(to_string(from))},
             {"to", std::string(to_string(to))}})
            ->increment();
    }
}

} // namespace tb::resilience
