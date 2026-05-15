#pragma once
/**
 * @file bitget_rest_client.hpp
 * @brief Синхронный HTTP клиент для Bitget REST API v2
 *
 * Использует Boost.Beast + SSL для выполнения аутентифицированных
 * запросов к REST API. Создаёт новое TCP/SSL соединение на каждый запрос.
 *
 * Production-grade:
 *  - Retry с экспоненциальным backoff для transient HTTP ошибок (5xx, timeout)
 *  - Clock sync check: сверка локального времени с биржей
 *  - Token bucket rate limiter (10 req/sec)
 *  - Idempotent retry: безопасен для POST (Bitget дедуплицирует по clientOid)
 */

#include "bitget_signing.hpp"
#include "logging/logger.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tb::exchange::bitget {

/// Результат HTTP-запроса к Bitget REST API
struct RestResponse {
    int status_code{0};
    std::string body;
    bool success{false};
    std::string error_message;
    std::string error_code;   ///< Bitget API error code (поле "code" из JSON)
};

/**
 * @brief Синхронный REST клиент для Bitget API v2
 *
 * Каждый вызов создаёт новое SSL-соединение (простой и надёжный подход).
 * Подписывает запросы с помощью make_auth_headers().
 * Retry при transient ошибках: 5xx, timeout, network errors.
 */
class BitgetRestClient {
public:
    BitgetRestClient(
        std::string base_url,       // "https://api.bitget.com"
        std::string api_key,
        std::string api_secret,
        std::string passphrase,
        std::shared_ptr<logging::ILogger> logger,
        int timeout_ms = 5000
    );

    ~BitgetRestClient();

    /// GET запрос с аутентификацией и retry
    RestResponse get(const std::string& path, const std::string& query_params = "");

    /// POST запрос с аутентификацией и retry
    RestResponse post(const std::string& path, const std::string& json_body);

    /// DELETE запрос с аутентификацией и retry
    RestResponse del(const std::string& path, const std::string& json_body = "");

    /// Проверить синхронизацию часов с биржей.
    /// @return Смещение в миллисекундах (local - server). Положительное = локальные часы впереди.
    /// @throws std::runtime_error если запрос не удался после retry.
    int64_t check_clock_sync();

    /// Получить серверное время Bitget (milliseconds since epoch).
    /// Не требует аутентификации.
    int64_t get_server_time_ms();

    /// Текущее смещение локальных часов относительно сервера (ms).
    /// Обновляется при вызове check_clock_sync().
    [[nodiscard]] int64_t clock_offset_ms() const { return clock_offset_ms_.load(); }

private:
    /// Выполнить единичный HTTP-запрос (без retry)
    RestResponse execute_once(const std::string& method, const std::string& path,
                              const std::string& body);

    /// Выполнить HTTP-запрос с retry при transient ошибках
    RestResponse execute(const std::string& method, const std::string& path,
                         const std::string& body);

    /// Определить, является ли ошибка transient (можно retry)
    static bool is_transient_error(const RestResponse& resp);

    /// Per-endpoint rate limiter: блокирует до появления токена.
    /// Bitget лимит: 10 req/sec для order endpoints, 20 req/sec для query endpoints.
    void wait_for_rate_limit(const std::string& path);

    /// Determine rate limit category from API path
    enum class RateCategory { Order, Query, Public };
    static RateCategory categorize_path(const std::string& path);

    std::string base_url_;
    std::string host_;      ///< Извлечённый хост (api.bitget.com)
    std::string port_;      ///< "443" для HTTPS
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::shared_ptr<logging::ILogger> logger_;
    int timeout_ms_;

    /// Retry policy
    static constexpr int kMaxRetries = 3;
    static constexpr int kBaseBackoffMs = 200;    ///< Начальная задержка retry
    static constexpr int kMaxBackoffMs = 3000;    ///< Максимальная задержка retry

    /// Clock sync state
    std::atomic<int64_t> clock_offset_ms_{0};

    /// Per-endpoint token bucket state
    struct TokenBucket {
        double max_tokens;
        double refill_rate;      ///< tokens per second
        double tokens;
        std::chrono::steady_clock::time_point last_refill{std::chrono::steady_clock::now()};

        explicit TokenBucket(double max_tok = 10.0, double rate = 10.0)
            : max_tokens(max_tok), refill_rate(rate), tokens(max_tok) {}
    };
    mutable std::mutex rate_mutex_;
    std::unordered_map<int, TokenBucket> rate_buckets_;  ///< keyed by RateCategory

    /// Persistent connection state (connection pooling)
    struct ConnectionPool;
    std::unique_ptr<ConnectionPool> conn_pool_;
    mutable std::mutex conn_pool_mutex_;
};

} // namespace tb::exchange::bitget
