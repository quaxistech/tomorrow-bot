/**
 * @file retry_executor.cpp
 * @brief Реализация retry executor (нешаблонные методы)
 */

#include "resilience/retry_executor.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace tb::resilience {

namespace {
    constexpr const char* kComponent = "RetryExecutor";
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
// execute_simple — retry с парой (status_code, body)
// ============================================================

std::pair<int, std::string> RetryExecutor::execute_simple(
    const std::string& operation_name,
    std::function<std::pair<int, std::string>()> operation)
{
    std::lock_guard lock(mutex_);
    last_attempts_.clear();

    std::pair<int, std::string> last_result{0, ""};

    for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
        // Проверяем circuit breaker
        if (!breaker_->allow_request()) {
            logger_->warn(kComponent, "Circuit breaker открыт, операция отклонена",
                {{"operation", operation_name},
                 {"circuit", breaker_->name()},
                 {"state", std::string(to_string(breaker_->state()))}});

            ExecutionAttempt ea;
            ea.attempt_number = attempt;
            ea.success = false;
            ea.error_message = "Circuit breaker open";
            ea.error_class = ErrorClassification::Transient;
            last_attempts_.push_back(std::move(ea));
            break;
        }

        const auto start_ts = clock_->now();

        last_result = operation();

        const auto end_ts = clock_->now();
        const int64_t latency_ms = (end_ts.get() - start_ts.get()) / 1'000'000;

        const int status = last_result.first;
        const auto& body = last_result.second;
        const bool ok = (status >= 200 && status < 300);

        const auto error_class = ok
            ? ErrorClassification::Unknown
            : classify_error(status, body);

        ExecutionAttempt ea;
        ea.attempt_number = attempt;
        ea.success = ok;
        ea.http_status = status;
        ea.error_message = ok ? "" : body;
        ea.latency_ms = latency_ms;
        ea.error_class = error_class;
        last_attempts_.push_back(ea);

        // Метрики
        if (metrics_) {
            metrics_->counter("resilience_attempts_total",
                {{"operation", operation_name},
                 {"success", ok ? "true" : "false"}})
                ->increment();

            if (!ok) {
                metrics_->counter("resilience_errors_total",
                    {{"operation", operation_name},
                     {"class", std::string(to_string(error_class))}})
                    ->increment();
            }
        }

        if (ok) {
            breaker_->record_success();
            logger_->debug(kComponent, "Операция выполнена успешно",
                {{"operation", operation_name},
                 {"attempt", std::to_string(attempt)},
                 {"latency_ms", std::to_string(latency_ms)}});
            return last_result;
        }

        breaker_->record_failure();

        // Не retry для permanent/auth ошибок
        if (error_class == ErrorClassification::Permanent ||
            error_class == ErrorClassification::AuthFailure) {
            logger_->error(kComponent, "Неретрируемая ошибка, прерывание",
                {{"operation", operation_name},
                 {"attempt", std::to_string(attempt)},
                 {"status", std::to_string(status)},
                 {"class", std::string(to_string(error_class))}});
            return last_result;
        }

        // Последняя попытка — не ждём
        if (attempt == config_.max_retries) {
            logger_->error(kComponent, "Исчерпаны все попытки",
                {{"operation", operation_name},
                 {"attempts", std::to_string(attempt + 1)},
                 {"status", std::to_string(status)}});
            return last_result;
        }

        // Вычисляем задержку
        int64_t delay = compute_delay(attempt);

        // Для RateLimit увеличиваем backoff в 3 раза
        if (error_class == ErrorClassification::RateLimit) {
            delay *= 3;
        }

        logger_->warn(kComponent, "Retry после ошибки",
            {{"operation", operation_name},
             {"attempt", std::to_string(attempt)},
             {"status", std::to_string(status)},
             {"class", std::string(to_string(error_class))},
             {"delay_ms", std::to_string(delay)}});

        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    return last_result;
}

// ============================================================
// last_attempts — история попыток последней операции
// ============================================================

const std::vector<ExecutionAttempt>& RetryExecutor::last_attempts() const {
    return last_attempts_;
}

// ============================================================
// classify_error — классификация HTTP ошибки
// ============================================================

ErrorClassification RetryExecutor::classify_error(
    int http_status, [[maybe_unused]] const std::string& body)
{
    if (http_status == 0) {
        return ErrorClassification::Transient;   // Сетевая ошибка / timeout
    }
    if (http_status == 429) {
        return ErrorClassification::RateLimit;    // Rate limit
    }
    if (http_status == 401 || http_status == 403) {
        return ErrorClassification::AuthFailure;  // Ошибка авторизации
    }
    if (http_status >= 500) {
        return ErrorClassification::Transient;    // Серверная ошибка
    }
    if (http_status >= 400) {
        return ErrorClassification::Permanent;    // Клиентская ошибка
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
