/**
 * @file bitget_rest_client.cpp
 * @brief Реализация синхронного REST клиента для Bitget API v2
 */

#include "bitget_rest_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace tb::exchange::bitget {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp       = net::ip::tcp;

static constexpr char kComp[] = "BitgetRest";

// Извлечение host и port из URL вида https://host[:port]
static void parse_base_url(const std::string& url,
                           std::string& host, std::string& port) {
    std::string rest = url;
    // Убрать схему
    if (auto pos = rest.find("://"); pos != std::string::npos) {
        rest = rest.substr(pos + 3);
    }
    // Убрать trailing slash
    if (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    // Разделить host:port
    if (auto colon = rest.find(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    } else {
        host = rest;
        port = "443";
    }
}

BitgetRestClient::BitgetRestClient(
    std::string base_url,
    std::string api_key,
    std::string api_secret,
    std::string passphrase,
    std::shared_ptr<logging::ILogger> logger,
    int timeout_ms)
    : base_url_(std::move(base_url))
    , api_key_(std::move(api_key))
    , api_secret_(std::move(api_secret))
    , passphrase_(std::move(passphrase))
    , logger_(std::move(logger))
    , timeout_ms_(timeout_ms)
{
    parse_base_url(base_url_, host_, port_);
    logger_->info(kComp, "REST клиент создан",
        {{"host", host_}, {"port", port_}, {"timeout_ms", std::to_string(timeout_ms_)}});
}

BitgetRestClient::~BitgetRestClient() = default;

RestResponse BitgetRestClient::get(const std::string& path,
                                    const std::string& query_params) {
    // Для GET: подписываем path + query_params, тело пустое
    std::string full_path = path;
    if (!query_params.empty()) {
        full_path += "?" + query_params;
    }
    return execute("GET", full_path, "");
}

RestResponse BitgetRestClient::post(const std::string& path,
                                     const std::string& json_body) {
    return execute("POST", path, json_body);
}

RestResponse BitgetRestClient::del(const std::string& path,
                                    const std::string& json_body) {
    return execute("DELETE", path, json_body);
}

RestResponse BitgetRestClient::execute(const std::string& method,
                                        const std::string& path,
                                        const std::string& body) {
    RestResponse response;

    try {
        // 1. Подписать запрос
        auto auth = make_auth_headers(api_key_, api_secret_, passphrase_,
                                       method, path, body);

        // 2. Настроить SSL контекст
        ssl::context ssl_ctx{ssl::context::tlsv12_client};
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3);

        // 3. Резолвер и соединение
        net::io_context ioc;
        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ssl_ctx};

        // SNI для TLS
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
            throw std::runtime_error("Не удалось установить SNI");
        }

        // Таймаут на уровне TCP
        beast::get_lowest_layer(stream).expires_after(
            std::chrono::milliseconds(timeout_ms_));

        auto results = resolver.resolve(host_, port_);
        beast::get_lowest_layer(stream).connect(results);

        // 4. SSL рукопожатие
        stream.handshake(ssl::stream_base::client);

        // 5. Формируем HTTP-запрос
        auto verb = http::string_to_verb(method);
        http::request<http::string_body> req{verb, path, 11};
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TomorrowBot/2.0");
        req.set(http::field::content_type, "application/json");
        req.set("locale", "en-US");
        req.set("ACCESS-KEY", auth.access_key);
        req.set("ACCESS-SIGN", auth.signature);
        req.set("ACCESS-TIMESTAMP", auth.timestamp);
        req.set("ACCESS-PASSPHRASE", auth.passphrase);

        if (!body.empty()) {
            req.body() = body;
            req.prepare_payload();
        }

        logger_->debug(kComp, "Запрос: " + method + " " + path,
            {{"body_len", std::to_string(body.size())}});

        // 6. Отправить запрос
        http::write(stream, req);

        // 7. Прочитать ответ
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        response.status_code = static_cast<int>(res.result_int());
        response.body = res.body();
        response.success = (response.status_code >= 200 && response.status_code < 300);

        logger_->debug(kComp, "Ответ: " + std::to_string(response.status_code),
            {{"body_len", std::to_string(response.body.size())}});

        if (!response.success) {
            response.error_message = "HTTP " + std::to_string(response.status_code);
            logger_->warn(kComp, "HTTP ошибка",
                {{"status", std::to_string(response.status_code)},
                 {"body", response.body.substr(0, 512)}});
        }

        // 8. Корректное закрытие SSL
        beast::error_code ec;
        stream.shutdown(ec);
        // Игнорируем ошибки при shutdown — они нормальны для SSL

    } catch (const std::exception& ex) {
        response.success = false;
        response.error_message = ex.what();
        logger_->error(kComp, "Ошибка запроса: " + std::string(ex.what()),
            {{"method", method}, {"path", path}});
    }

    return response;
}

} // namespace tb::exchange::bitget
