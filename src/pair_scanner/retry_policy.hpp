#pragma once

/**
 * @file retry_policy.hpp
 * @brief Политика повторных попыток и circuit breaker для API-вызовов.
 *
 * Реализует:
 * - Экспоненциальный backoff с jitter для transient ошибок
 * - Circuit breaker для защиты от каскадных отказов
 * - Классификацию ошибок на transient/permanent
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace tb::pair_scanner {

// ─────────────────────────────────────────────────────────────────────────────
// Retry Policy
// ─────────────────────────────────────────────────────────────────────────────

/// Результат классификации ошибки
enum class ErrorClass {
    Transient,   ///< Временная ошибка (5xx, timeout, network) — можно retry
    Permanent    ///< Постоянная ошибка (4xx, auth, invalid request) — retry бесполезен
};

/// Классифицировать HTTP-ошибку
inline ErrorClass classify_http_error(int status_code) {
    if (status_code == 0)   return ErrorClass::Transient;  // network/timeout
    if (status_code >= 500) return ErrorClass::Transient;
    if (status_code == 429) return ErrorClass::Transient;  // rate limit
    if (status_code == 408) return ErrorClass::Transient;  // request timeout
    return ErrorClass::Permanent;
}

/// Политика retry с экспоненциальным backoff
struct RetryPolicy {
    int max_retries{3};
    int base_delay_ms{200};
    double backoff_multiplier{2.0};
    int max_delay_ms{5000};

    /// Вычислить задержку для i-й попытки (с jitter)
    int delay_for_attempt(int attempt) const {
        double delay = base_delay_ms * std::pow(backoff_multiplier, attempt);
        delay = std::min(delay, static_cast<double>(max_delay_ms));

        // Jitter ±25%
        thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<> dist(0.75, 1.25);
        return static_cast<int>(delay * dist(gen));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Circuit Breaker
// ─────────────────────────────────────────────────────────────────────────────

/// Состояние circuit breaker
enum class CircuitState {
    Closed,     ///< Нормальная работа
    Open,       ///< Запросы блокируются
    HalfOpen    ///< Пробный запрос разрешён
};

/// Circuit breaker для защиты от каскадных отказов API
class CircuitBreaker {
public:
    CircuitBreaker(int failure_threshold, int reset_timeout_ms)
        : failure_threshold_(failure_threshold)
        , reset_timeout_ms_(reset_timeout_ms)
    {}

    /// Проверить, разрешён ли запрос
    bool allow_request() {
        auto now = std::chrono::steady_clock::now();
        auto state = get_state(now);

        if (state == CircuitState::Closed) return true;
        if (state == CircuitState::HalfOpen) return true;
        return false;  // Open — запросы блокируются
    }

    /// Зафиксировать успешный запрос
    void record_success() {
        failure_count_.store(0);
        state_.store(static_cast<int>(CircuitState::Closed));
    }

    /// Зафиксировать неудачный запрос
    void record_failure() {
        int count = failure_count_.fetch_add(1) + 1;
        if (count >= failure_threshold_) {
            state_.store(static_cast<int>(CircuitState::Open));
            last_failure_time_ = std::chrono::steady_clock::now();
        }
    }

    /// Получить текущее состояние
    CircuitState current_state() const {
        return get_state(std::chrono::steady_clock::now());
    }

    /// Количество накопленных ошибок
    int failure_count() const { return failure_count_.load(); }

private:
    CircuitState get_state(std::chrono::steady_clock::time_point now) const {
        auto raw = static_cast<CircuitState>(state_.load());
        if (raw == CircuitState::Open) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_failure_time_).count();
            if (elapsed >= reset_timeout_ms_) {
                return CircuitState::HalfOpen;
            }
        }
        return raw;
    }

    int failure_threshold_;
    int reset_timeout_ms_;
    std::atomic<int> failure_count_{0};
    std::atomic<int> state_{static_cast<int>(CircuitState::Closed)};
    std::chrono::steady_clock::time_point last_failure_time_;
};

} // namespace tb::pair_scanner
