/**
 * @file bitget_rest_client.cpp
 * @brief Реализация синхронного REST клиента для Bitget API v2
 *
 * Production-grade: retry с exponential backoff, clock sync, token bucket rate limiter.
 */

#include "bitget_rest_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

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
    if (auto pos = rest.find("://"); pos != std::string::npos) {
        rest = rest.substr(pos + 3);
    }
    if (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
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

    // Initialize per-category rate buckets
    // Bitget: order endpoints 10 req/sec, query endpoints 20 req/sec, public 20 req/sec
    rate_buckets_.emplace(static_cast<int>(RateCategory::Order), TokenBucket(10.0, 10.0));
    rate_buckets_.emplace(static_cast<int>(RateCategory::Query), TokenBucket(20.0, 20.0));
    rate_buckets_.emplace(static_cast<int>(RateCategory::Public), TokenBucket(20.0, 20.0));

    // Initialize connection pool
    conn_pool_ = std::make_unique<ConnectionPool>();

    logger_->info(kComp, "REST клиент создан",
        {{"host", host_}, {"port", port_}, {"timeout_ms", std::to_string(timeout_ms_)}});
}

BitgetRestClient::~BitgetRestClient() = default;

// ==================== Connection Pool ====================

/// Persistent SSL connection for connection reuse (keep-alive)
struct BitgetRestClient::ConnectionPool {
    std::unique_ptr<net::io_context> ioc;
    std::unique_ptr<ssl::context> ssl_ctx;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;
    bool connected{false};
    std::chrono::steady_clock::time_point last_used{};
    int requests_on_connection{0};
    static constexpr int kMaxRequestsPerConnection = 100;
    static constexpr auto kMaxIdleTime = std::chrono::seconds(25);
};

// ==================== Rate limiter ====================

BitgetRestClient::RateCategory BitgetRestClient::categorize_path(const std::string& path) {
    // Order placement/cancel endpoints: 10 req/sec
    if (path.find("/order/place") != std::string::npos ||
        path.find("/order/cancel") != std::string::npos ||
        path.find("/order/batch") != std::string::npos ||
        path.find("/order/close") != std::string::npos ||
        path.find("/order/reversal") != std::string::npos) {
        return RateCategory::Order;
    }
    // Public endpoints
    if (path.find("/public/") != std::string::npos) {
        return RateCategory::Public;
    }
    // Everything else (queries, position, account): 20 req/sec
    return RateCategory::Query;
}

void BitgetRestClient::wait_for_rate_limit(const std::string& path) {
    auto cat = categorize_path(path);
    std::unique_lock<std::mutex> lock(rate_mutex_);
    auto& bucket = rate_buckets_.at(static_cast<int>(cat));

    while (true) {
        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - bucket.last_refill).count();

        bucket.tokens = std::min(bucket.max_tokens, bucket.tokens + elapsed_sec * bucket.refill_rate);
        bucket.last_refill = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return;
        }

        double wait_sec = (1.0 - bucket.tokens) / bucket.refill_rate;
        auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(wait_sec));

        logger_->debug(kComp, "Rate limit: ожидание токена",
            {{"wait_ms", std::to_string(wait_duration.count())},
             {"category", std::to_string(static_cast<int>(cat))}});

        lock.unlock();
        std::this_thread::sleep_for(wait_duration);
        lock.lock();
    }
}

// ==================== Transient error detection ====================

bool BitgetRestClient::is_transient_error(const RestResponse& resp) {
    // Network/connection errors (status_code == 0 means no HTTP response received)
    if (resp.status_code == 0) return true;

    // Server-side transient errors
    if (resp.status_code == 500 || resp.status_code == 502 ||
        resp.status_code == 503 || resp.status_code == 504) {
        return true;
    }

    // Rate limit exceeded (429) — should be rare with token bucket, but handle gracefully
    if (resp.status_code == 429) return true;

    return false;
}

// ==================== Public API ====================

RestResponse BitgetRestClient::get(const std::string& path,
                                    const std::string& query_params) {
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

// ==================== Clock sync ====================

int64_t BitgetRestClient::get_server_time_ms() {
    // Public endpoint — no auth needed but still passes through execute_once
    // for consistent connection handling
    wait_for_rate_limit("/api/v2/public/time");
    RestResponse resp;

    try {
        ssl::context ssl_ctx{ssl::context::tlsv12_client};
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3);

        net::io_context ioc;
        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ssl_ctx};

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
            throw std::runtime_error("SNI setup failed");
        }

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::milliseconds(timeout_ms_));

        auto results = resolver.resolve(host_, port_);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::get, "/api/v2/public/time", 11};
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TomorrowBot/2.0");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

        if (static_cast<int>(res.result_int()) != 200) {
            throw std::runtime_error("Server time request failed: HTTP "
                + std::to_string(res.result_int()));
        }

        auto json = boost::json::parse(res.body());
        auto& root = json.as_object();
        std::string code = std::string(root.at("code").as_string());
        if (code != "00000") {
            throw std::runtime_error("Server time API error: code=" + code);
        }

        // Bitget: { "data": { "serverTime": "1234567890123" } }
        // or:     { "data": "1234567890123" }
        const auto& data_val = root.at("data");
        if (data_val.is_object()) {
            auto st = std::string(data_val.as_object().at("serverTime").as_string());
            return std::stoll(st);
        }
        if (data_val.is_string()) {
            return std::stoll(std::string(data_val.as_string()));
        }

        throw std::runtime_error("Unexpected server time response format");

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Не удалось получить серверное время: " + std::string(ex.what()), {});
        throw;
    }
}

int64_t BitgetRestClient::check_clock_sync() {
    auto before = std::chrono::system_clock::now();
    int64_t server_ms = get_server_time_ms();
    auto after = std::chrono::system_clock::now();

    // Estimate local time at server response (midpoint of request)
    auto local_mid = before + (after - before) / 2;
    int64_t local_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        local_mid.time_since_epoch()).count();

    int64_t offset = local_ms - server_ms;
    clock_offset_ms_.store(offset);

    if (std::abs(offset) > 1000) {
        logger_->warn(kComp, "Значительное расхождение часов с биржей",
            {{"offset_ms", std::to_string(offset)},
             {"server_ms", std::to_string(server_ms)},
             {"local_ms", std::to_string(local_ms)}});
    } else {
        logger_->info(kComp, "Clock sync OK",
            {{"offset_ms", std::to_string(offset)}});
    }

    return offset;
}

// ==================== Retry wrapper ====================

RestResponse BitgetRestClient::execute(const std::string& method,
                                        const std::string& path,
                                        const std::string& body) {
    // Thread-local RNG for jitter — no mutex needed
    thread_local std::mt19937 rng(std::random_device{}());

    RestResponse response;

    for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
        // Rate limit: ждём доступный токен перед каждой попыткой
        wait_for_rate_limit(path);

        response = execute_once(method, path, body);

        // Success or non-transient error — return immediately
        if (response.success || !is_transient_error(response)) {
            return response;
        }

        // Last attempt — don't sleep, just return the error
        if (attempt == kMaxRetries) {
            logger_->error(kComp, "Все retry исчерпаны",
                {{"method", method}, {"path", path},
                 {"attempts", std::to_string(attempt + 1)},
                 {"last_status", std::to_string(response.status_code)},
                 {"last_error", response.error_message}});
            break;
        }

        // Exponential backoff with jitter: base * 3^attempt + uniform[-50, +50]ms
        int backoff_ms = kBaseBackoffMs * static_cast<int>(std::pow(3, attempt));
        backoff_ms = std::min(backoff_ms, kMaxBackoffMs);
        std::uniform_int_distribution<int> jitter(-50, 50);
        backoff_ms = std::max(50, backoff_ms + jitter(rng));

        logger_->warn(kComp, "Transient error, retrying",
            {{"attempt", std::to_string(attempt + 1)},
             {"max_retries", std::to_string(kMaxRetries)},
             {"backoff_ms", std::to_string(backoff_ms)},
             {"status", std::to_string(response.status_code)},
             {"error", response.error_message},
             {"path", path}});

        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    }

    return response;
}

// ==================== Single HTTP request execution ====================

RestResponse BitgetRestClient::execute_once(const std::string& method,
                                             const std::string& path,
                                             const std::string& body) {
    RestResponse response;

    try {
        // 1. Подписать запрос (fresh timestamp every attempt — critical for retry)
        auto auth = make_auth_headers(api_key_, api_secret_, passphrase_,
                                       method, path, body,
                                       clock_offset_ms_.load(std::memory_order_relaxed));

        // 2. Check if persistent connection is still usable
        auto& pool = *conn_pool_;
        auto now = std::chrono::steady_clock::now();
        bool need_reconnect = !pool.connected
            || pool.requests_on_connection >= ConnectionPool::kMaxRequestsPerConnection
            || (now - pool.last_used) > ConnectionPool::kMaxIdleTime;

        if (need_reconnect) {
            // Tear down old connection
            if (pool.stream) {
                beast::error_code ec;
                pool.stream->shutdown(ec);
                pool.stream.reset();
            }
            pool.ioc.reset();
            pool.ssl_ctx.reset();
            pool.connected = false;
            pool.requests_on_connection = 0;

            // Create new persistent connection
            pool.ssl_ctx = std::make_unique<ssl::context>(ssl::context::tlsv12_client);
            pool.ssl_ctx->set_verify_mode(ssl::verify_peer);
            pool.ssl_ctx->set_default_verify_paths();
            pool.ssl_ctx->set_options(
                ssl::context::default_workarounds |
                ssl::context::no_sslv2 |
                ssl::context::no_sslv3);

            pool.ioc = std::make_unique<net::io_context>();
            pool.stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
                *pool.ioc, *pool.ssl_ctx);

            if (!SSL_set_tlsext_host_name(pool.stream->native_handle(), host_.c_str())) {
                throw std::runtime_error("Не удалось установить SNI");
            }

            beast::get_lowest_layer(*pool.stream).expires_after(
                std::chrono::milliseconds(timeout_ms_));

            tcp::resolver resolver{*pool.ioc};
            auto results = resolver.resolve(host_, port_);
            beast::get_lowest_layer(*pool.stream).connect(results);
            pool.stream->handshake(ssl::stream_base::client);
            pool.connected = true;

            logger_->debug(kComp, "Новое SSL соединение установлено", {});
        }

        // 3. Reset timeout for this request
        beast::get_lowest_layer(*pool.stream).expires_after(
            std::chrono::milliseconds(timeout_ms_));

        // 4. Формируем HTTP-запрос
        auto verb = http::string_to_verb(method);
        http::request<http::string_body> req{verb, path, 11};
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TomorrowBot/2.0");
        req.set(http::field::content_type, "application/json");
        req.set(http::field::connection, "keep-alive");
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

        // 5. Отправить запрос
        http::write(*pool.stream, req);

        // 6. Прочитать ответ
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(*pool.stream, buffer, res);

        pool.last_used = std::chrono::steady_clock::now();
        ++pool.requests_on_connection;

        // Check if server wants to close connection
        if (res[http::field::connection] == "close") {
            pool.connected = false;
        }

        response.status_code = static_cast<int>(res.result_int());
        response.body = res.body();
        response.success = (response.status_code >= 200 && response.status_code < 300);

        logger_->debug(kComp, "Ответ: " + std::to_string(response.status_code),
            {{"body_len", std::to_string(response.body.size())},
             {"reused", std::to_string(pool.requests_on_connection)}});

        if (!response.success) {
            response.error_message = "HTTP " + std::to_string(response.status_code);
            if (!response.body.empty()) {
                try {
                    auto err_json = boost::json::parse(response.body);
                    auto& err_obj = err_json.as_object();
                    if (err_obj.contains("code")) {
                        response.error_code = std::string(err_obj.at("code").as_string());
                        response.error_message += " [code=" + response.error_code + "]";
                    }
                    if (err_obj.contains("msg")) {
                        response.error_message += " " + std::string(err_obj.at("msg").as_string());
                    }
                } catch (...) {
                    // JSON parsing failed — keep HTTP-level error
                }
            }
            logger_->warn(kComp, "HTTP ошибка",
                {{"status", std::to_string(response.status_code)},
                 {"error_code", response.error_code},
                 {"body", response.body.substr(0, 512)}});
        }

    } catch (const std::exception& ex) {
        // Connection failed — mark pool as disconnected for next attempt
        conn_pool_->connected = false;
        response.success = false;
        response.status_code = 0;  // Mark as network-level failure (enables retry)
        response.error_message = ex.what();
        logger_->error(kComp, "Ошибка запроса: " + std::string(ex.what()),
            {{"method", method}, {"path", path}});
    }

    return response;
}

} // namespace tb::exchange::bitget
