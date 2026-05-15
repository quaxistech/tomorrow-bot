/**
 * @file retry_executor.cpp
 * @brief Реализация retry executor (нешаблонные методы)
 */

#include "resilience/retry_executor.hpp"

#include <algorithm>
#include <boost/json.hpp>
#include <cmath>
#include <random>

namespace tb::resilience {

namespace {

/// @brief Обёртка для делегирования execute_simple → execute
struct SimpleResponse {
    bool success{false};
    int code{0};
    std::string msg;
};

} // namespace

// ============================================================
// Конструктор
// ============================================================

RetryExecutor::RetryExecutor(
    RetryConfig retry_config,
    std::shared_ptr<CircuitBreaker> breaker,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(retry_config))
    , breaker_(std::move(breaker))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

// ============================================================
// execute_simple — делегирует в шаблонный execute()
// ============================================================

std::pair<int, std::string> RetryExecutor::execute_simple(
    const std::string& operation_name,
    std::function<std::pair<int, std::string>()> operation)
{
    auto result = execute(operation_name, [&operation]() -> SimpleResponse {
        auto [status, body] = operation();
        return {status >= 200 && status < 300, status, std::move(body)};
    });
    return {result.code, std::move(result.msg)};
}

// ============================================================
// last_attempts — потокобезопасная копия истории попыток
// ============================================================

std::vector<ExecutionAttempt> RetryExecutor::last_attempts() const {
    std::lock_guard lock(mutex_);
    return last_attempts_;
}

// ============================================================
// classify_error — классификация HTTP ошибки с Bitget API awareness
// ============================================================

ErrorClassification RetryExecutor::classify_error(
    int http_status, const std::string& body)
{
    // BUG-S19-05 fix: parse JSON to extract the "code" field rather than using
    // substring search, which can misclassify permanent errors if the error
    // code digits happen to appear in a message string.
    if (!body.empty()) {
        try {
            auto jv = boost::json::parse(body);
            auto code_sv = jv.at("code").as_string();
            std::string code(code_sv.begin(), code_sv.end());
            // Rate limit: Bitget code 43011 "too many requests"
            if (code == "43011") {
                return ErrorClassification::RateLimit;
            }
            // Auth failures: invalid key, IP whitelist, expired key
            if (code == "40014" || code == "40016" || code == "40017") {
                return ErrorClassification::AuthFailure;
            }
        } catch (...) {
            // Body is not valid JSON (e.g., plain text error from proxy) — fall through
        }
    }

    // HTTP status-based classification
    if (http_status == 0) {
        return ErrorClassification::Transient;
    }
    if (http_status == 429) {
        return ErrorClassification::RateLimit;
    }
    if (http_status == 401 || http_status == 403) {
        return ErrorClassification::AuthFailure;
    }
    if (http_status >= 500) {
        return ErrorClassification::Transient;
    }
    if (http_status >= 400) {
        return ErrorClassification::Permanent;
    }
    return ErrorClassification::Unknown;
}

// ============================================================
// compute_delay — экспоненциальный backoff с jitter
// ============================================================

int64_t RetryExecutor::compute_delay(int attempt) const {
    // base_delay * 2^attempt
    const double raw_delay = static_cast<double>(config_.base_delay_ms)
        * std::pow(2.0, static_cast<double>(attempt));

    // Jitter: случайная добавка [0, jitter_factor * raw_delay]
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<double> dist(0.0, config_.jitter_factor);
    const double jitter = raw_delay * dist(gen);

    const double total = raw_delay + jitter;

    // Ограничиваем max_delay
    return static_cast<int64_t>(
        std::min(total, static_cast<double>(config_.max_delay_ms)));
}

} // namespace tb::resilience
