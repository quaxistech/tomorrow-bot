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
#include <memory>
#include <string>

namespace tb::exchange::bitget {

/// Результат HTTP-запроса к Bitget REST API
struct RestResponse {
    int status_code{0};
    std::string body;
    bool success{false};
    std::string error_message;
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

    std::string base_url_;
    std::string host_;      ///< Извлечённый хост (api.bitget.com)
    std::string port_;      ///< "443" для HTTPS
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::shared_ptr<logging::ILogger> logger_;
    int timeout_ms_;
};

} // namespace tb::exchange::bitget
