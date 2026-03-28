/**
 * @file resilience_types.hpp
 * @brief Типы и конфигурации модуля отказоустойчивости
 *
 * Определяет конфигурации retry policy, circuit breaker,
 * классификацию ошибок и структуры для идемпотентности ордеров.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tb::resilience {

// ============================================================
// Конфигурация retry policy
// ============================================================

/// @brief Конфигурация retry policy
struct RetryConfig {
    int max_retries{3};
    int64_t base_delay_ms{200};
    int64_t max_delay_ms{10000};
    double jitter_factor{0.3};          ///< Случайная добавка до 30% от delay
    bool retry_on_timeout{true};
};

// ============================================================
// Конфигурация circuit breaker
// ============================================================

/// @brief Конфигурация circuit breaker
struct CircuitBreakerConfig {
    int failure_threshold{5};           ///< Переход Open после N отказов
    int64_t recovery_timeout_ms{30000}; ///< Время в Open перед HalfOpen
    int half_open_max_attempts{2};      ///< Попытки в HalfOpen
};

// ============================================================
// Состояние circuit breaker
// ============================================================

/// @brief Состояние circuit breaker
enum class CircuitState { Closed, Open, HalfOpen };

/// @brief Строковое представление CircuitState
[[nodiscard]] inline std::string_view to_string(CircuitState s) noexcept {
    switch (s) {
        case CircuitState::Closed:   return "Closed";
        case CircuitState::Open:     return "Open";
        case CircuitState::HalfOpen: return "HalfOpen";
    }
    return "Unknown";
}

// ============================================================
// Классификация ошибок (расширенная)
// ============================================================

/// @brief Классификация ошибок для retry-логики
enum class ErrorClassification {
    Transient,          ///< Timeout, 5xx, network — retry
    RateLimit,          ///< 429 — retry с увеличенным backoff
    Permanent,          ///< 4xx (кроме 429) — не retry
    AuthFailure,        ///< 401/403 — критическая, kill switch
    Unknown             ///< Неклассифицированная
};

/// @brief Строковое представление ErrorClassification
[[nodiscard]] inline std::string_view to_string(ErrorClassification ec) noexcept {
    switch (ec) {
        case ErrorClassification::Transient:   return "Transient";
        case ErrorClassification::RateLimit:   return "RateLimit";
        case ErrorClassification::Permanent:   return "Permanent";
        case ErrorClassification::AuthFailure: return "AuthFailure";
        case ErrorClassification::Unknown:     return "Unknown";
    }
    return "Unknown";
}

// ============================================================
// Результат выполнения с retry
// ============================================================

/// @brief Результат одной попытки выполнения операции
struct ExecutionAttempt {
    int attempt_number{0};
    bool success{false};
    int http_status{0};
    std::string error_message;
    int64_t latency_ms{0};
    ErrorClassification error_class{ErrorClassification::Unknown};
};

// ============================================================
// Конфигурация идемпотентности ордеров
// ============================================================

/// @brief Конфигурация идемпотентности ордеров
struct IdempotencyConfig {
    bool enabled{true};
    std::string client_id_prefix{"tb"};
    int64_t dedup_window_ms{300000};    ///< 5 минут — окно дедупликации
};

} // namespace tb::resilience
