#pragma once
#include "bitget_models.hpp"
#include "logging/logger.hpp"
#include <functional>
#include <memory>
#include <string>
#include <atomic>

namespace tb::exchange::bitget {

using MessageCallback    = std::function<void(RawWsMessage)>;
using ConnectionCallback = std::function<void(bool connected)>;

struct WsClientConfig {
    std::string url{"wss://ws.bitget.com/v2/ws/public"};
    int reconnect_delay_ms{1000};
    int max_reconnect_delay_ms{30000};
    int heartbeat_interval_sec{25};
    int connect_timeout_ms{10000};
};

// Асинхронный WebSocket клиент для Bitget (pimpl)
// НЕ содержит логику стратегий или исполнения ордеров
class BitgetWsClient {
public:
    explicit BitgetWsClient(WsClientConfig config,
                            MessageCallback on_message,
                            ConnectionCallback on_connection,
                            std::shared_ptr<tb::logging::ILogger> logger);
    ~BitgetWsClient();

    // Некопируемый, неперемещаемый (владеет потоком io_context)
    BitgetWsClient(const BitgetWsClient&)            = delete;
    BitgetWsClient& operator=(const BitgetWsClient&) = delete;

    void start();
    void stop();
    void subscribe(const std::string& channel);
    void unsubscribe(const std::string& channel);
    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tb::exchange::bitget
