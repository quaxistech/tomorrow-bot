/**
 * @file retry_executor.hpp
 * @brief Универсальный retry executor с circuit breaker и jitter
 *
 * Выполняет операции с автоматическим retry, circuit breaker
 * и экспоненциальным backoff. Шаблонный метод execute() позволяет
 * работать с любыми REST-клиентами без жёстких зависимостей.
 */
#pragma once

#include "resilience/resilience_types.hpp"
#include "resilience/circuit_breaker.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace tb::resilience {

/// @brief Универсальный retry executor с circuit breaker и jitter
class RetryExecutor {
public:
    RetryExecutor(
        RetryConfig retry_config,
        std::shared_ptr<CircuitBreaker> breaker,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /**
     * @brief Выполнить операцию с retry и circuit breaker
     *
     * F должен возвращать тип с полями:
     *   - bool   success        (или code == 0 для успеха)
     *   - int    code / status  (HTTP статус-код)
     *   - string msg / message  (текст ошибки)
     *
     * @tparam F  Callable: () -> ResponseType
     * @param operation_name  Имя операции для логов и метрик
     * @param operation       Вызываемый объект
     * @return Результат последней попытки
     */
    template<typename F>
    auto execute(const std::string& operation_name, F&& operation)
        -> decltype(operation());

    /**
     * @brief Выполнить операцию с retry (упрощённый вариант)
     *
     * Принимает callable, возвращающий pair<int, string> (status_code, body).
     * Код 200-299 считается успехом.
     *
     * @param operation_name  Имя операции для логов и метрик
     * @param operation       Callable: () -> pair<int, string>
     * @return pair<int, string> — результат последней попытки
     */
    std::pair<int, std::string> execute_simple(
        const std::string& operation_name,
        std::function<std::pair<int, std::string>()> operation);

    /// @brief Получить историю попыток последней операции
    [[nodiscard]] const std::vector<ExecutionAttempt>& last_attempts() const;

    /// @brief Классифицировать HTTP ошибку
    [[nodiscard]] static ErrorClassification classify_error(
        int http_status, const std::string& body);

private:
    /// @brief Рассчитать задержку с экспоненциальным backoff и jitter
    [[nodiscard]] int64_t compute_delay(int attempt) const;

    RetryConfig config_;
    std::shared_ptr<CircuitBreaker> breaker_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    std::vector<ExecutionAttempt> last_attempts_;
    mutable std::mutex mutex_;
};

// ============================================================
// Шаблонная реализация execute()
// ============================================================

namespace detail {

/// @brief Трейт для извлечения http-статуса из ответа
template<typename T, typename = void>
struct has_code : std::false_type {};

template<typename T>
struct has_code<T, std::void_t<decltype(std::declval<T>().code)>> : std::true_type {};

template<typename T, typename = void>
struct has_status_code : std::false_type {};

template<typename T>
struct has_status_code<T, std::void_t<decltype(std::declval<T>().status_code)>> : std::true_type {};

/// Извлечь HTTP статус из ответа
template<typename T>
int extract_status(const T& response) {
    if constexpr (has_code<T>::value) {
        return static_cast<int>(response.code);
    } else if constexpr (has_status_code<T>::value) {
        return static_cast<int>(response.status_code);
    } else {
        return 0;
    }
}

/// @brief Трейт для извлечения сообщения об ошибке
template<typename T, typename = void>
struct has_msg : std::false_type {};

template<typename T>
struct has_msg<T, std::void_t<decltype(std::declval<T>().msg)>> : std::true_type {};

template<typename T, typename = void>
struct has_message : std::false_type {};

template<typename T>
struct has_message<T, std::void_t<decltype(std::declval<T>().message)>> : std::true_type {};

template<typename T, typename = void>
struct has_error_message : std::false_type {};

template<typename T>
struct has_error_message<T, std::void_t<decltype(std::declval<T>().error_message)>> : std::true_type {};

/// Извлечь сообщение об ошибке из ответа
template<typename T>
std::string extract_message(const T& response) {
    if constexpr (has_msg<T>::value) {
        return std::string(response.msg);
    } else if constexpr (has_message<T>::value) {
        return std::string(response.message);
    } else if constexpr (has_error_message<T>::value) {
        return std::string(response.error_message);
    } else {
        return {};
    }
}

/// @brief Трейт для проверки наличия поля success
template<typename T, typename = void>
struct has_success : std::false_type {};

template<typename T>
struct has_success<T, std::void_t<decltype(std::declval<T>().success)>> : std::true_type {};

/// Проверить успешность ответа
template<typename T>
bool is_success(const T& response) {
    if constexpr (has_success<T>::value) {
        return static_cast<bool>(response.success);
    } else {
        int status = extract_status(response);
        return status >= 200 && status < 300;
    }
}

} // namespace detail

template<typename F>
auto RetryExecutor::execute(const std::string& operation_name, F&& operation)
    -> decltype(operation())
{
    using ResponseType = decltype(operation());

    std::unique_lock lock(mutex_);
    last_attempts_.clear();

    constexpr const char* kComponent = "RetryExecutor";

    ResponseType last_response{};

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

        // Засекаем время
        const auto start_ts = clock_->now();

        // Выполняем операцию
        last_response = operation();

        const auto end_ts = clock_->now();
        const int64_t latency_ms = (end_ts.get() - start_ts.get()) / 1'000'000;

        // Извлекаем данные из ответа
        const int status = detail::extract_status(last_response);
        const std::string msg = detail::extract_message(last_response);
        const bool ok = detail::is_success(last_response);

        // Классифицируем ошибку
        const auto error_class = ok
            ? ErrorClassification::Unknown
            : classify_error(status, msg);

        // Записываем попытку
        ExecutionAttempt ea;
        ea.attempt_number = attempt;
        ea.success = ok;
        ea.http_status = status;
        ea.error_message = msg;
        ea.latency_ms = latency_ms;
        ea.error_class = error_class;
        last_attempts_.push_back(ea);

        // Обновляем метрики
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
            return last_response;
        }

        // Неуспешно
        breaker_->record_failure();

        // Не retry для permanent/auth ошибок
        if (error_class == ErrorClassification::Permanent ||
            error_class == ErrorClassification::AuthFailure) {
            logger_->error(kComponent, "Неретрируемая ошибка, прерывание",
                {{"operation", operation_name},
                 {"attempt", std::to_string(attempt)},
                 {"status", std::to_string(status)},
                 {"class", std::string(to_string(error_class))},
                 {"msg", msg}});
            return last_response;
        }

        // Последняя попытка — не ждём
        if (attempt == config_.max_retries) {
            logger_->error(kComponent, "Исчерпаны все попытки",
                {{"operation", operation_name},
                 {"attempts", std::to_string(attempt + 1)},
                 {"status", std::to_string(status)},
                 {"msg", msg}});
            return last_response;
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
             {"delay_ms", std::to_string(delay)},
             {"msg", msg}});

        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        lock.lock();
    }

    return last_response;
}

} // namespace tb::resilience
