/**
 * @file http_server.hpp
 * @brief Lightweight HTTP endpoint server for Prometheus metrics
 *
 * Single-threaded async Boost.Beast HTTP server that exposes
 * application metrics in Prometheus text format.
 * Binds to loopback only (127.0.0.1) by default for security.
 */
#pragma once

#include "logging/logger.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <utility>
#include <vector>

namespace tb::app {

/// HTTP response structure returned by the request handler callback
struct HttpResponse {
    int         status_code{200};
    std::string status_text{"OK"};
    std::string content_type{"text/plain"};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// Request handler: (method, target) -> HttpResponse
using HttpRequestHandler = std::function<HttpResponse(std::string_view method,
                                                       std::string_view target)>;

/**
 * @brief Lightweight HTTP server for metrics / health endpoints
 *
 * Runs a Boost.Beast acceptor on a dedicated io_context thread.
 * Each accepted connection is handled synchronously in the io_context.
 * Designed for low-frequency monitoring requests (Prometheus scrapes).
 */
class HttpEndpointServer {
public:
    /**
     * @param bind_address  IP to bind (e.g. "127.0.0.1")
     * @param port          TCP port
     * @param server_name   Identifier for logging
     * @param handler       Callback invoked for every HTTP request
     * @param logger        Logger instance
     */
    HttpEndpointServer(std::string bind_address,
                       uint16_t port,
                       std::string server_name,
                       HttpRequestHandler handler,
                       std::shared_ptr<logging::ILogger> logger);

    ~HttpEndpointServer();

    HttpEndpointServer(const HttpEndpointServer&)            = delete;
    HttpEndpointServer& operator=(const HttpEndpointServer&) = delete;
    HttpEndpointServer(HttpEndpointServer&&)                  = delete;
    HttpEndpointServer& operator=(HttpEndpointServer&&)       = delete;

    /// Start the server (non-blocking). Returns true on success.
    [[nodiscard]] bool start();

    /// Stop the server and join the worker thread.
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

private:
    void run_accept_loop(std::promise<bool>& bind_result);
    void handle_connection(boost::asio::ip::tcp::socket socket);

    std::string                        bind_address_;
    uint16_t                           port_;
    std::string                        server_name_;
    HttpRequestHandler                 handler_;
    std::shared_ptr<logging::ILogger>  logger_;

    boost::asio::io_context            io_ctx_;
    /// ИСПРАВЛЕНИЕ H7: acceptor как member — для корректного shutdown из stop()
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::mutex                         acceptor_mutex_;
    std::atomic<bool>                  running_{false};
    std::thread                        worker_thread_;
};

} // namespace tb::app
