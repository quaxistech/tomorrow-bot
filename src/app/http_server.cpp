/**
 * @file http_server.cpp
 * @brief Implementation of the lightweight HTTP endpoint server
 */
#include "http_server.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <future>

namespace tb::app {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

// ===========================================================================
// Construction / Destruction
// ===========================================================================

HttpEndpointServer::HttpEndpointServer(std::string bind_address,
                                       uint16_t port,
                                       std::string server_name,
                                       HttpRequestHandler handler,
                                       std::shared_ptr<logging::ILogger> logger)
    : bind_address_(std::move(bind_address))
    , port_(port)
    , server_name_(std::move(server_name))
    , handler_(std::move(handler))
    , logger_(std::move(logger))
{}

HttpEndpointServer::~HttpEndpointServer() {
    stop();
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool HttpEndpointServer::start() {
    if (running_.exchange(true)) {
        return true; // already running
    }

    // ИСПРАВЛЕНИЕ H7: start() ждёт реального bind/listen через promise.
    // Если bind не удался — возвращаем false немедленно, не оставляя зомби-поток.
    std::promise<bool> bind_promise;
    auto bind_future = bind_promise.get_future();

    try {
        worker_thread_ = std::thread([this, &bind_promise]() {
            try {
                run_accept_loop(bind_promise);
            } catch (const std::exception& e) {
                if (logger_) {
                    logger_->error(server_name_,
                        "HTTP server fatal error: " + std::string(e.what()));
                }
            }
        });

        // Ждём результат bind/listen (таймаут 5 секунд)
        auto status = bind_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::ready && bind_future.get()) {
            return true;
        }
        // bind не удался или таймаут
        running_.store(false);
        {
            std::lock_guard lock(acceptor_mutex_);
            if (acceptor_ && acceptor_->is_open()) {
                boost::system::error_code ec;
                acceptor_->close(ec);
            }
        }
        if (worker_thread_.joinable()) worker_thread_.join();
        return false;
    } catch (const std::exception& e) {
        running_.store(false);
        if (logger_) {
            logger_->error(server_name_,
                "Failed to start HTTP server thread: " + std::string(e.what()));
        }
        return false;
    }
}

void HttpEndpointServer::stop() {
    if (!running_.exchange(false)) {
        return; // not running
    }
    // ИСПРАВЛЕНИЕ H7: закрываем acceptor ДО io_ctx_.stop().
    // Это прерывает блокирующий accept() с ec = operation_aborted.
    {
        std::lock_guard lock(acceptor_mutex_);
        if (acceptor_ && acceptor_->is_open()) {
            boost::system::error_code ec;
            acceptor_->close(ec);
        }
    }
    io_ctx_.stop();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

// ===========================================================================
// Accept loop
// ===========================================================================

void HttpEndpointServer::run_accept_loop(std::promise<bool>& bind_result) {
    beast::error_code ec;
    auto const address = net::ip::make_address(bind_address_, ec);
    if (ec) {
        if (logger_) {
            logger_->error(server_name_,
                "Invalid bind address: " + bind_address_,
                {{"error", ec.message()}});
        }
        running_.store(false);
        bind_result.set_value(false);
        return;
    }

    tcp::endpoint endpoint{address, port_};

    {
        std::lock_guard lock(acceptor_mutex_);
        acceptor_ = std::make_unique<tcp::acceptor>(io_ctx_);
    }

    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        if (logger_) {
            logger_->error(server_name_, "Socket open failed",
                {{"error", ec.message()}});
        }
        running_.store(false);
        bind_result.set_value(false);
        return;
    }

    acceptor_->set_option(net::socket_base::reuse_address(true), ec);
    acceptor_->bind(endpoint, ec);
    if (ec) {
        if (logger_) {
            logger_->error(server_name_, "Bind failed",
                {{"address", bind_address_},
                 {"port", std::to_string(port_)},
                 {"error", ec.message()}});
        }
        running_.store(false);
        bind_result.set_value(false);
        return;
    }

    acceptor_->listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        if (logger_) {
            logger_->error(server_name_, "Listen failed",
                {{"error", ec.message()}});
        }
        running_.store(false);
        bind_result.set_value(false);
        return;
    }

    // Bind/listen успешен — сигнализируем start()
    bind_result.set_value(true);

    while (running_.load()) {
        tcp::socket socket{io_ctx_};
        acceptor_->accept(socket, ec);
        if (ec) {
            if (running_.load()) {
                if (logger_) {
                    logger_->debug(server_name_, "Accept error",
                        {{"error", ec.message()}});
                }
            }
            continue;
        }
        handle_connection(std::move(socket));
    }
}

// ===========================================================================
// Connection handler
// ===========================================================================

void HttpEndpointServer::handle_connection(tcp::socket socket) {
    beast::error_code ec;
    beast::flat_buffer buffer;

    // Read one HTTP request
    http::request<http::string_body> req;
    http::read(socket, buffer, req, ec);
    if (ec) {
        return; // client disconnected or malformed request
    }

    // Invoke the application handler
    const std::string method_str = std::string(req.method_string());
    const std::string target_str = std::string(req.target());

    HttpResponse app_response;
    try {
        app_response = handler_(method_str, target_str);
    } catch (const std::exception& e) {
        app_response.status_code = 500;
        app_response.status_text = "Internal Server Error";
        app_response.body = "Internal Server Error\n";
        if (logger_) {
            logger_->error(server_name_, "Handler exception",
                {{"error", e.what()},
                 {"method", method_str},
                 {"target", target_str}});
        }
    }

    // Build Beast HTTP response
    http::response<http::string_body> res{
        static_cast<http::status>(app_response.status_code), req.version()};
    res.set(http::field::server, "TomorrowBot/2.0");
    res.set(http::field::content_type, app_response.content_type);
    res.keep_alive(false);

    for (const auto& [key, value] : app_response.headers) {
        res.set(key, value);
    }

    res.body() = std::move(app_response.body);
    res.prepare_payload();

    http::write(socket, res, ec);

    // Graceful close
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace tb::app
