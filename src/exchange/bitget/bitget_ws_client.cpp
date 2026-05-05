#include "bitget_ws_client.hpp"

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
#include <unordered_map>
#include <vector>

namespace tb::exchange::bitget {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;

static constexpr char kComp[] = "BitgetWsClient";

// WebSocket поверх SSL/TLS
using WsStream = websocket::stream<ssl::stream<tcp::socket>>;

// -----------------------------------------------------------------------
// Разбор URL вида wss://host[:port]/path
struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string path{"/"};
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl r;
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
struct BitgetWsClient::Impl {
    WsClientConfig       config;
    MessageCallback      on_message;
    ConnectionCallback   on_connection;
    std::shared_ptr<tb::logging::ILogger> logger;

    net::io_context      ioc;
    ssl::context         ssl_ctx{ssl::context::tlsv12_client};
    std::thread          io_thread;

    net::steady_timer    reconnect_timer;
    net::steady_timer    heartbeat_timer;

    // Создаётся заново при каждом подключении
    std::unique_ptr<WsStream> ws_stream;
    beast::flat_buffer   read_buf;

    std::atomic<bool>    connected{false};
    std::atomic<bool>    running{false};

    // BUG-S23-02: track when last pong was received to detect dead connections
    std::atomic<int64_t> last_pong_ms_{0};

    std::mutex           subs_mutex;
    std::vector<std::string> subscriptions;
    // BUG-S23-03: track subscriptions pending confirmation with send timestamp
    std::unordered_map<std::string, int64_t> pending_sub_ms_;  // channel → sent_ms

    /// Очередь сообщений для последовательной отправки (Boost.Beast требует)
    std::vector<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};

    int reconnect_attempt{0};
    ParsedUrl url_parts;

    // ----------------------------------------------------------------
    explicit Impl(WsClientConfig cfg,
                  MessageCallback msg_cb,
                  ConnectionCallback conn_cb,
                  std::shared_ptr<tb::logging::ILogger> log)
        : config(std::move(cfg))
        , on_message(std::move(msg_cb))
        , on_connection(std::move(conn_cb))
        , logger(std::move(log))
        , reconnect_timer(ioc)
        , heartbeat_timer(ioc)
        , url_parts(parse_url(config.url))
    {
        // TLS 1.2 клиент — не требует отключения SSLv2/v3
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_default_verify_paths();
    }

    // ----------------------------------------------------------------
    void connect() {
        ws_stream = std::make_unique<WsStream>(ioc, ssl_ctx);

        // SNI для корректного TLS рукопожатия
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

    // ----------------------------------------------------------------
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            logger->error(kComp, "resolve: " + ec.message());
            schedule_reconnect();
            return;
        }
        net::async_connect(
            beast::get_lowest_layer(*ws_stream),
            results,
            [this](beast::error_code ec, tcp::endpoint) {
                on_tcp_connect(ec);
            }
        );
    }

    // ----------------------------------------------------------------
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

    // ----------------------------------------------------------------
    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            logger->error(kComp, "ssl_handshake: " + ec.message());
            schedule_reconnect();
            return;
        }
        // Не используем timeout::suggested() — он может конфликтовать
        // с нашим heartbeat механизмом на уровне приложения
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

    // ----------------------------------------------------------------
    void on_ws_handshake(beast::error_code ec) {
        if (ec) {
            logger->error(kComp, "ws_handshake: " + ec.message());
            schedule_reconnect();
            return;
        }
        connected.store(true, std::memory_order_release);
        reconnect_attempt = 0;
        // BUG-S23-02: initialize pong timestamp so first heartbeat check doesn't immediately fire
        last_pong_ms_.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_release);
        logger->info(kComp, "Подключён: " + config.url);

        // Отправляем ранее накопленные подписки
        {
            std::lock_guard lock(subs_mutex);
            for (const auto& ch : subscriptions) do_subscribe_direct(ch);
        }

        // Уведомляем шлюз (он может добавить новые подписки)
        if (on_connection) on_connection(true);

        schedule_heartbeat();
        do_read();
    }

    // ----------------------------------------------------------------
    void do_read() {
        read_buf.clear();
        ws_stream->async_read(
            read_buf,
            [this](beast::error_code ec, std::size_t /*bytes*/) {
                on_read(ec);
            }
        );
    }

    // ----------------------------------------------------------------
    void on_read(beast::error_code ec) {
        if (ec) {
            if (ec != net::error::operation_aborted) {
                handle_disconnect("read: " + ec.message());
            }
            return;
        }

        try {
            auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            std::string payload = beast::buffers_to_string(read_buf.data());

            RawWsMessage raw{
                .type        = WsMsgType::Unknown,
                .raw_payload = payload,
                .received_ns = now_ns
            };

            if (payload == "pong" || payload.find("\"pong\"") != std::string::npos) {
                raw.type = WsMsgType::Heartbeat;
                // BUG-S23-02: record pong receipt time for dead-connection detection
                last_pong_ms_.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_release);
            } else {
                // Структурный разбор JSON: определяем тип по arg.channel
                try {
                    auto jv = boost::json::parse(payload);
                    auto& obj = jv.as_object();

                    if (obj.contains("event")) {
                        auto ev = obj.at("event").as_string();
                        if (ev == "subscribe") {
                            raw.type = WsMsgType::Subscribe;
                            // BUG-S23-03: mark subscription as confirmed
                            if (obj.contains("arg") && obj.at("arg").is_object()) {
                                const auto& arg = obj.at("arg").as_object();
                                if (arg.contains("channel")) {
                                    std::string ch(arg.at("channel").as_string());
                                    std::lock_guard lock(subs_mutex);
                                    pending_sub_ms_.erase(ch);
                                }
                            }
                        }
                        else if (ev == "error") raw.type = WsMsgType::Error;
                    } else if (obj.contains("arg")) {
                        auto& arg = obj.at("arg").as_object();
                        if (arg.contains("channel")) {
                            auto ch = arg.at("channel").as_string();
                            if (ch == "ticker") raw.type = WsMsgType::Ticker;
                            else if (ch == "trade") raw.type = WsMsgType::Trade;
                            else if (ch.starts_with("books")) raw.type = WsMsgType::OrderBook;
                            else if (ch.starts_with("candle")) raw.type = WsMsgType::Candle;
                        }
                    }
                } catch (...) {
                    // JSON parse failure — fallback to substring classification
                    if (payload.find("\"error\"") != std::string::npos) {
                        raw.type = WsMsgType::Error;
                    }
                }
            }

            if (on_message) on_message(std::move(raw));
        } catch (const std::exception& e) {
            logger->error(kComp, std::string("on_read: ") + e.what());
        }

        read_buf.consume(read_buf.size());
        do_read();
    }

    // ----------------------------------------------------------------
    void handle_disconnect(const std::string& reason) {
        if (!connected.exchange(false)) return;
        logger->warn(kComp, "Разрыв: " + reason);
        heartbeat_timer.cancel();

        // Очищаем очередь записи — все сообщения для старого соединения бесполезны
        write_queue_.clear();
        writing_ = false;

        // Сбрасываем старый стрим, чтобы предотвратить попытки записи
        // на мёртвый сокет до создания нового подключения
        ws_stream.reset();

        if (on_connection) on_connection(false);
        if (running.load()) schedule_reconnect();
    }

    // ----------------------------------------------------------------
    void schedule_reconnect() {
        if (!running.load()) return;

        int delay_ms = config.reconnect_delay_ms;
        for (int i = 0; i < reconnect_attempt && delay_ms < config.max_reconnect_delay_ms; ++i) {
            delay_ms = std::min(delay_ms * 2, config.max_reconnect_delay_ms);
        }
        ++reconnect_attempt;

        logger->info(kComp,
            "Переподключение через " + std::to_string(delay_ms) +
            "мс (попытка " + std::to_string(reconnect_attempt) + ")"
        );

        reconnect_timer.expires_after(std::chrono::milliseconds(delay_ms));
        reconnect_timer.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running.load()) connect();
        });
    }

    // ----------------------------------------------------------------
    void schedule_heartbeat() {
        if (!running.load()) return;
        heartbeat_timer.expires_after(
            std::chrono::seconds(config.heartbeat_interval_sec)
        );
        heartbeat_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec || !running.load()) return;
            if (!connected.load()) return;

            // BUG-S23-02: check that pong arrived within 1.5× heartbeat interval.
            // A missing pong means the TCP connection is dead (no FIN/RST from broker).
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t pong_age_ms = now_ms - last_pong_ms_.load(std::memory_order_acquire);
            const int64_t pong_timeout_ms = config.heartbeat_interval_sec * 1500;  // 1.5×
            if (pong_age_ms > pong_timeout_ms) {
                logger->error(kComp, "Pong timeout (" + std::to_string(pong_age_ms) +
                    "ms) — dead connection, reconnecting");
                handle_disconnect("pong_timeout");
                return;
            }

            // BUG-S23-03: retry subscriptions that haven't been confirmed within 5 seconds.
            // Collect retry targets under the lock, then retry outside to avoid deadlock
            // (do_subscribe_direct also acquires subs_mutex for pending_sub_ms_).
            std::vector<std::string> retry_channels;
            {
                std::lock_guard lock(subs_mutex);
                for (auto& [ch, sent_ms] : pending_sub_ms_) {
                    if (now_ms - sent_ms > 5000) {
                        for (const auto& sub_json : subscriptions) {
                            try {
                                auto jv = boost::json::parse(sub_json);
                                if (jv.is_object() && jv.as_object().contains("channel")) {
                                    if (std::string(jv.as_object().at("channel").as_string()) == ch) {
                                        retry_channels.push_back(sub_json);
                                        break;
                                    }
                                }
                            } catch (...) {}
                        }
                    }
                }
            }
            for (const auto& sub_json : retry_channels) {
                logger->warn(kComp, "Subscription unconfirmed after 5s — retrying");
                do_subscribe_direct(sub_json);
            }

            do_send_on_strand("ping");
            schedule_heartbeat();
        });
    }

    // ----------------------------------------------------------------
    // Очередь записи — Boost.Beast запрещает параллельные async_write.
    // Все вызовы do_send_on_strand() ДОЛЖНЫ выполняться из io_context потока.
    void do_send_on_strand(std::string payload) {
        if (!connected.load() || !ws_stream) return;
        auto buf = std::make_shared<std::string>(std::move(payload));
        write_queue_.push_back(buf);
        if (!writing_) {
            do_write_next();
        }
    }

    /// Безопасный вызов из любого потока — перенаправляет на io_context
    void do_send(std::string payload) {
        if (!connected.load() || !ws_stream) return;
        net::post(ioc, [this, p = std::move(payload)]() mutable {
            do_send_on_strand(std::move(p));
        });
    }

    void do_write_next() {
        if (write_queue_.empty() || !ws_stream || !connected.load()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto buf = write_queue_.front();
        write_queue_.erase(write_queue_.begin());
        ws_stream->async_write(
            net::buffer(*buf),
            [this, buf](beast::error_code ec, std::size_t /*bytes*/) {
                if (ec) {
                    logger->error(kComp, "async_write: " + ec.message());
                    writing_ = false;
                    // КРИТИЧНО: вызываем handle_disconnect, чтобы запустить
                    // переподключение. Без этого connected остаётся true,
                    // heartbeat продолжает слать на мёртвый сокет, и бот
                    // зависает в бесконечном цикле "Operation canceled".
                    handle_disconnect("write_error: " + ec.message());
                    return;
                }
                do_write_next();
            }
        );
    }

    // ----------------------------------------------------------------
    /// Отправить подписку (вызывать ТОЛЬКО из io_context потока)
    /// channel — JSON-объект подписки, например: {"instType":"SPOT","channel":"ticker","instId":"BTCUSDT"}
    void do_subscribe_direct(const std::string& channel) {
        auto msg = R"({"op":"subscribe","args":[)" + channel + R"(]})";
        logger->info(kComp, "Подписка: " + msg);
        do_send_on_strand(std::move(msg));

        // BUG-S23-03: record subscription as pending confirmation
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        try {
            auto jv = boost::json::parse(channel);
            if (jv.is_object() && jv.as_object().contains("channel")) {
                std::string ch(jv.as_object().at("channel").as_string());
                pending_sub_ms_[ch] = now_ms;
            }
        } catch (...) {}  // channel may not be JSON-parseable as standalone object
    }

    /// Отправить отписку (вызывать ТОЛЬКО из io_context потока)
    void do_unsubscribe_direct(const std::string& channel) {
        do_send_on_strand(R"({"op":"unsubscribe","args":[)" + channel + R"(]})");
    }

    // ----------------------------------------------------------------
    void close_gracefully() {
        if (!ws_stream || !connected.load()) return;
        ws_stream->async_close(
            websocket::close_code::going_away,
            [](beast::error_code) {}
        );
    }
};

// -----------------------------------------------------------------------
// Публичный интерфейс
// -----------------------------------------------------------------------

BitgetWsClient::BitgetWsClient(WsClientConfig config,
                               MessageCallback on_message,
                               ConnectionCallback on_connection,
                               std::shared_ptr<tb::logging::ILogger> logger)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(on_message),
          std::move(on_connection),
          std::move(logger)
      ))
{}

BitgetWsClient::~BitgetWsClient() {
    stop();
}

void BitgetWsClient::start() {
    impl_->running.store(true, std::memory_order_release);
    impl_->connect();
    impl_->io_thread = std::thread([this] {
        try {
            impl_->ioc.run();
        } catch (const std::exception& e) {
            impl_->logger->error(kComp, std::string("io_context: ") + e.what());
        }
    });
}

void BitgetWsClient::stop() {
    if (!impl_->running.exchange(false)) return;

    impl_->reconnect_timer.cancel();
    impl_->heartbeat_timer.cancel();
    impl_->close_gracefully();
    impl_->ioc.stop();

    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }
}

void BitgetWsClient::subscribe(const std::string& channel) {
    {
        std::lock_guard lock(impl_->subs_mutex);
        auto& subs = impl_->subscriptions;
        if (std::find(subs.begin(), subs.end(), channel) != subs.end()) return;
        subs.push_back(channel);
    }
    if (impl_->connected.load()) {
        net::post(impl_->ioc, [this, channel] {
            impl_->do_subscribe_direct(channel);
        });
    }
}

void BitgetWsClient::unsubscribe(const std::string& channel) {
    {
        std::lock_guard lock(impl_->subs_mutex);
        auto& subs = impl_->subscriptions;
        subs.erase(std::remove(subs.begin(), subs.end(), channel), subs.end());
    }
    if (impl_->connected.load()) {
        net::post(impl_->ioc, [this, channel] {
            impl_->do_unsubscribe_direct(channel);
        });
    }
}

bool BitgetWsClient::is_connected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

} // namespace tb::exchange::bitget
