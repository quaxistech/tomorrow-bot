#pragma once
/**
 * @file bitget_rest_client.hpp
 * @brief Синхронный HTTP клиент для Bitget REST API v2
 *
 * Использует Boost.Beast + SSL для выполнения аутентифицированных
 * запросов к REST API. Создаёт новое TCP/SSL соединение на каждый запрос.
 */

#include "bitget_signing.hpp"
#include "logging/logger.hpp"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

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

    /// GET запрос с аутентификацией
    RestResponse get(const std::string& path, const std::string& query_params = "");

    /// POST запрос с аутентификацией
    RestResponse post(const std::string& path, const std::string& json_body);

    /// DELETE запрос с аутентификацией
    RestResponse del(const std::string& path, const std::string& json_body = "");

private:
    /// Выполнить HTTP-запрос (внутренняя реализация)
    RestResponse execute(const std::string& method, const std::string& path,
                         const std::string& body);

    /// Token bucket rate limiter: блокирует до появления токена.
    /// Bitget лимит: 10 запросов/сек.
    void wait_for_rate_limit();

    std::string base_url_;
    std::string host_;      ///< Извлечённый хост (api.bitget.com)
    std::string port_;      ///< "443" для HTTPS
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::shared_ptr<logging::ILogger> logger_;
    int timeout_ms_;

    /// Token bucket rate limiter state
    mutable std::mutex rate_mutex_;
    static constexpr double kMaxTokens = 10.0;       ///< Макс. токенов (Bitget: 10 req/sec)
    static constexpr double kRefillRate = 10.0;       ///< Токенов в секунду
    double tokens_{kMaxTokens};                        ///< Текущее кол-во токенов
    std::chrono::steady_clock::time_point last_refill_{std::chrono::steady_clock::now()};
};

} // namespace tb::exchange::bitget
