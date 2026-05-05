#include "bitget_private_ws_client.hpp"
#include "bitget_signing.hpp"
#include <openssl/crypto.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tb::exchange::bitget {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

static constexpr char kComp[] = "BitgetPrivateWs";

using WsStream = websocket::stream<ssl::stream<tcp::socket>>;

// -----------------------------------------------------------------------
struct ParsedPrivateUrl {
    std::string host;
    std::string port{"443"};
    std::string path{"/"};
};

static ParsedPrivateUrl parse_private_url(const std::string& url) {
    ParsedPrivateUrl r;
    std::string rest = url;
    const std::string prefix = "wss://";
    if (rest.substr(0, prefix.size()) == prefix) rest = rest.substr(prefix.size());
    auto slash = rest.find('/');
    std::string host_port = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    if (slash != std::string::npos) r.path = rest.substr(slash);
    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        r.host = host_port.substr(0, colon);
        r.port = host_port.substr(colon + 1);
    } else {
        r.host = host_port;
    }
    return r;
}

// -----------------------------------------------------------------------
struct BitgetPrivateWsClient::Impl {
    PrivateWsConfig      config;
    PrivateMessageCallback on_message;
    PrivateConnectionCallback on_connection;
    std::shared_ptr<tb::logging::ILogger> logger;

    net::io_context      ioc;
    ssl::context         ssl_ctx{ssl::context::tlsv12_client};
    std::thread          io_thread;

    net::steady_timer    reconnect_timer;
    net::steady_timer    heartbeat_timer;
    net::steady_timer    auth_timer;     // BUG-S23-04: enforces 10s auth timeout

    std::unique_ptr<WsStream> ws_stream;
    beast::flat_buffer   read_buf;

    std::atomic<bool>    connected{false};
    std::atomic<bool>    authenticated{false};
    std::atomic<bool>    running{false};

    std::mutex           subs_mutex;
    std::vector<std::pair<std::string, std::string>> subscriptions;  // {inst_type, channel}

    std::vector<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};

    int reconnect_attempt{0};
    ParsedPrivateUrl url_parts;

    // ----------------------------------------------------------------
    explicit Impl(PrivateWsConfig cfg,
                  PrivateMessageCallback msg_cb,
                  PrivateConnectionCallback conn_cb,
                  std::shared_ptr<tb::logging::ILogger> log)
        : config(std::move(cfg))
        , on_message(std::move(msg_cb))
        , on_connection(std::move(conn_cb))
        , logger(std::move(log))
        , reconnect_timer(ioc)
        , heartbeat_timer(ioc)
        , auth_timer(ioc)
        , url_parts(parse_private_url(config.url))
    {
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();
    }

    // ----------------------------------------------------------------
    void connect() {
        ws_stream = std::make_unique<WsStream>(ioc, ssl_ctx);

        if (!SSL_set_tlsext_host_name(
                ws_stream->next_layer().native_handle(),
                url_parts.host.c_str())) {
            logger->warn(kComp, "SNI не удалось установить");
        }

        auto resolver = std::make_shared<tcp::resolver>(ioc);
        resolver->async_resolve(
            url_parts.host, url_parts.port,
            [this, resolver](beast::error_code ec, tcp::resolver::results_type res) {
                on_resolve(ec, std::move(res));
            }
        );
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            logger->error(kComp, "resolve: " + ec.message());
            schedule_reconnect();
            return;
        }
        net::async_connect(
            beast::get_lowest_layer(*ws_stream), results,
            [this](beast::error_code ec, tcp::endpoint) { on_tcp_connect(ec); }
        );
    }

    void on_tcp_connect(beast::error_code ec) {
        if (ec) {
            logger->error(kComp, "tcp_connect: " + ec.message());
            schedule_reconnect();
            return;
        }
        ws_stream->next_layer().async_handshake(
            ssl::stream_base::client,
            [this](beast::error_code ec) { on_ssl_handshake(ec); }
        );
    }

    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            logger->error(kComp, "ssl_handshake: " + ec.message());
            schedule_reconnect();
            return;
        }
        ws_stream->set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& req) {
                req.set(boost::beast::http::field::host, url_parts.host);
                req.set(boost::beast::http::field::user_agent, "TomorrowBot/2.0");
            }
        ));
        ws_stream->async_handshake(
            url_parts.host, url_parts.path,
            [this](beast::error_code ec) { on_ws_handshake(ec); }
        );
    }

    void on_ws_handshake(beast::error_code ec) {
        if (ec) {
            logger->error(kComp, "ws_handshake: " + ec.message());
            schedule_reconnect();
            return;
        }
        connected.store(true, std::memory_order_release);
        reconnect_attempt = 0;
        logger->info(kComp, "WS connected, sending login...");

        // BUG-S23-04: start auth timeout — disconnect and reconnect if no "login" event
        // arrives within 10 seconds to prevent the channel from blocking indefinitely.
        auth_timer.expires_after(std::chrono::seconds(10));
        auth_timer.async_wait([this](beast::error_code ec) {
            if (ec || !running.load(std::memory_order_relaxed)) return;
            if (!authenticated.load(std::memory_order_acquire)) {
                logger->error(kComp, "Auth timeout — reconnecting");
                schedule_reconnect();
            }
        });

        // Send login for authentication
        send_login();
        do_read();
    }

    // ----------------------------------------------------------------
    // Bitget private WS login: sign = Base64(HMAC-SHA256(secret, timestamp + "GET" + "/user/verify"))
    void send_login() {
        auto now = std::chrono::system_clock::now();
        auto epoch_s = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        std::string ts = std::to_string(epoch_s);

        std::string sign_payload = ts + "GET" + "/user/verify";
        std::string sign = hmac_sha256_base64(config.api_secret, sign_payload);

        boost::json::object arg;
        arg["apiKey"] = config.api_key;
        arg["passphrase"] = config.passphrase;
        arg["timestamp"] = ts;
        arg["sign"] = sign;

        boost::json::object login_msg;
        login_msg["op"] = "login";
        login_msg["args"] = boost::json::array{arg};

        enqueue_write(boost::json::serialize(login_msg));
        // BUG-S9-03: zero signing material after use to prevent it from remaining
        // accessible in memory dumps.
        OPENSSL_cleanse(sign_payload.data(), sign_payload.size());
        OPENSSL_cleanse(sign.data(), sign.size());
    }

    // ----------------------------------------------------------------
    void subscribe_channel(const std::string& inst_type, const std::string& channel) {
        boost::json::object arg;
        arg["instType"] = inst_type;
        arg["channel"] = channel;
        arg["instId"] = "default";

        boost::json::object sub_msg;
        sub_msg["op"] = "subscribe";
        sub_msg["args"] = boost::json::array{arg};

        enqueue_write(boost::json::serialize(sub_msg));
    }

    void resubscribe_all() {
        std::lock_guard lock(subs_mutex);
        for (const auto& [inst_type, channel] : subscriptions) {
            subscribe_channel(inst_type, channel);
        }
    }

    // ----------------------------------------------------------------
    void schedule_heartbeat() {
        if (!running.load(std::memory_order_relaxed)) return;
        heartbeat_timer.expires_after(
            std::chrono::seconds(config.heartbeat_interval_sec));
        heartbeat_timer.async_wait([this](beast::error_code ec) {
            if (ec || !running.load(std::memory_order_relaxed)) return;
            if (connected.load(std::memory_order_acquire)) {
                enqueue_write("ping");
            }
            schedule_heartbeat();
        });
    }

    void schedule_reconnect() {
        connected.store(false, std::memory_order_release);
        authenticated.store(false, std::memory_order_release);
        auth_timer.cancel();  // BUG-S23-04: ensure auth timer doesn't fire after reconnect
        if (on_connection) on_connection(false, false);
        if (!running.load(std::memory_order_relaxed)) return;

        int delay = std::min(
            config.reconnect_delay_ms * (1 << std::min(reconnect_attempt, 8)),
            config.max_reconnect_delay_ms);
        ++reconnect_attempt;

        logger->warn(kComp, "Reconnecting in " + std::to_string(delay) + "ms (attempt "
            + std::to_string(reconnect_attempt) + ")");

        reconnect_timer.expires_after(std::chrono::milliseconds(delay));
        reconnect_timer.async_wait([this](beast::error_code ec) {
            if (ec || !running.load(std::memory_order_relaxed)) return;
            connect();
        });
    }

    // ----------------------------------------------------------------
    void do_read() {
        if (!running.load(std::memory_order_relaxed)) return;
        ws_stream->async_read(read_buf,
            [this](beast::error_code ec, std::size_t /*bytes*/) {
                on_read(ec);
            }
        );
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            if (running.load(std::memory_order_relaxed)) {
                logger->warn(kComp, "read error: " + ec.message());
                schedule_reconnect();
            }
            return;
        }

        auto data = beast::buffers_to_string(read_buf.data());
        read_buf.consume(read_buf.size());

        // Handle pong
        if (data == "pong") {
            do_read();
            return;
        }

        try {
            auto json = boost::json::parse(data);
            auto& obj = json.as_object();

            // Handle login response
            if (obj.contains("event")) {
                std::string event(obj.at("event").as_string());
                if (event == "login") {
                    authenticated.store(true, std::memory_order_release);
                    auth_timer.cancel();  // BUG-S23-04: auth succeeded, cancel timeout
                    logger->info(kComp, "Authenticated successfully");
                    if (on_connection) on_connection(true, true);
                    resubscribe_all();
                    schedule_heartbeat();
                } else if (event == "error") {
                    std::string msg = obj.contains("msg")
                        ? std::string(obj.at("msg").as_string()) : "unknown";
                    logger->error(kComp, "Login/subscribe error: " + msg);
                }
                do_read();
                return;
            }

            // Handle data push — extract action type and dispatch
            if (obj.contains("action") && obj.contains("data")) {
                std::string action(obj.at("action").as_string());
                // Determine event type from arg channel
                std::string channel;
                if (obj.contains("arg") && obj.at("arg").is_object()) {
                    auto& arg = obj.at("arg").as_object();
                    if (arg.contains("channel"))
                        channel = std::string(arg.at("channel").as_string());
                }

                if (on_message) {
                    on_message(channel, obj.at("data"));
                }
            }
        } catch (const std::exception& e) {
            logger->warn(kComp, "Parse error: " + std::string(e.what()));
        }

        do_read();
    }

    // ----------------------------------------------------------------
    void enqueue_write(std::string msg) {
        auto sp = std::make_shared<std::string>(std::move(msg));
        net::post(ioc, [this, sp]() {
            write_queue_.push_back(sp);
            if (!writing_) do_write();
        });
    }

    void do_write() {
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto& front = write_queue_.front();
        ws_stream->async_write(
            net::buffer(*front),
            [this](beast::error_code ec, std::size_t) {
                if (ec) {
                    logger->warn(kComp, "write error: " + ec.message());
                    write_queue_.clear();
                    writing_ = false;
                    if (running.load(std::memory_order_relaxed)) schedule_reconnect();
                    return;
                }
                write_queue_.erase(write_queue_.begin());
                do_write();
            }
        );
    }

    // ----------------------------------------------------------------
    void do_close() {
        if (ws_stream && connected.load(std::memory_order_acquire)) {
            beast::error_code ec;
            ws_stream->close(websocket::close_code::normal, ec);
        }
        connected.store(false, std::memory_order_release);
        authenticated.store(false, std::memory_order_release);
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════════

BitgetPrivateWsClient::BitgetPrivateWsClient(
    PrivateWsConfig config,
    PrivateMessageCallback on_message,
    PrivateConnectionCallback on_connection,
    std::shared_ptr<tb::logging::ILogger> logger)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(on_message),
          std::move(on_connection), std::move(logger)))
{}

BitgetPrivateWsClient::~BitgetPrivateWsClient() {
    stop();
}

void BitgetPrivateWsClient::start() {
    if (impl_->running.exchange(true)) return;
    impl_->logger->info(kComp, "Starting private WS client");
    impl_->connect();
    impl_->io_thread = std::thread([this]() {
        impl_->ioc.run();
    });
}

void BitgetPrivateWsClient::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->logger->info(kComp, "Stopping private WS client");
    impl_->heartbeat_timer.cancel();
    impl_->reconnect_timer.cancel();
    // BUG-S9-04: posting do_close then immediately calling ioc.stop() races —
    // ioc may stop before do_close executes, leaving the fd open.
    // Fix: post a combined task that closes the WS and then stops the ioc,
    // ensuring the close handshake completes before the event loop exits.
    net::post(impl_->ioc, [this]() {
        impl_->do_close();
        impl_->ioc.stop();
    });
    if (impl_->io_thread.joinable()) impl_->io_thread.join();
}

void BitgetPrivateWsClient::subscribe(
    const std::string& inst_type, const std::string& channel) {
    {
        std::lock_guard lock(impl_->subs_mutex);
        impl_->subscriptions.emplace_back(inst_type, channel);
    }
    if (impl_->authenticated.load(std::memory_order_acquire)) {
        impl_->subscribe_channel(inst_type, channel);
    }
}

bool BitgetPrivateWsClient::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

bool BitgetPrivateWsClient::is_authenticated() const {
    return impl_->authenticated.load(std::memory_order_acquire);
}

} // namespace tb::exchange::bitget
