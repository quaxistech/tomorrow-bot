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
///
/// Научное обоснование дефолтов:
///   max_retries=3        — AWS SDK / Google Cloud SDK default (Google SRE Book §22)
///   base_delay_ms=100    — стандартный initial delay (AWS/GCP SDKs: 100ms)
///   max_delay_ms=5000    — потолок для скальпинга USDT-M futures (5s max)
///   jitter_factor=0.5    — "Equal Jitter" подход (Brooker, AWS Architecture Blog 2015)
///   rate_limit_multiplier — множитель backoff для 429 (Stripe best practice: 2-4x)
struct RetryConfig {
    int max_retries{3};
    int64_t base_delay_ms{100};
    int64_t max_delay_ms{5000};
    double jitter_factor{0.5};
    int rate_limit_backoff_multiplier{3};
};

// ============================================================
// Конфигурация circuit breaker
// ============================================================

/// @brief Конфигурация circuit breaker
///
/// Научное обоснование дефолтов:
///   failure_threshold=5       — Fowler Circuit Breaker pattern: 5-10 consecutive
///   recovery_timeout_ms=30000 — Microsoft Polly / Resilience4j default: 30s
///   half_open_max_attempts=3  — Resilience4j permittedNumberOfCallsInHalfOpenState
struct CircuitBreakerConfig {
    int failure_threshold{5};
    int64_t recovery_timeout_ms{30000};
    int half_open_max_attempts{3};
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
///
/// dedup_window_ms=300000 — 5 мин, стандартное окно TTL exchange order (Bitget USDT-M)
struct IdempotencyConfig {
    std::string client_id_prefix{"tb"};
    int64_t dedup_window_ms{300000};
};

} // namespace tb::resilience
