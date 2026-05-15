#pragma once
#include "logging/logger.hpp"
#include <boost/json/value.hpp>
#include <functional>
#include <memory>
#include <string>
#include <atomic>

namespace tb::exchange::bitget {

using PrivateMessageCallback    = std::function<void(const std::string& event_type, const boost::json::value& data)>;
using PrivateConnectionCallback = std::function<void(bool connected, bool authenticated)>;

struct PrivateWsConfig {
    std::string url{"wss://ws.bitget.com/v2/ws/private"};
    std::string api_key;
    std::string api_secret;
    std::string passphrase;
    int reconnect_delay_ms{1000};
    int max_reconnect_delay_ms{30000};
    int heartbeat_interval_sec{25};
    int connect_timeout_ms{10000};
};

/// Authenticated WebSocket client for Bitget private channels.
/// Subscribes to order updates, fill events, position changes.
class BitgetPrivateWsClient {
public:
    explicit BitgetPrivateWsClient(PrivateWsConfig config,
                                   PrivateMessageCallback on_message,
                                   PrivateConnectionCallback on_connection,
                                   std::shared_ptr<tb::logging::ILogger> logger);
    ~BitgetPrivateWsClient();

    BitgetPrivateWsClient(const BitgetPrivateWsClient&)            = delete;
    BitgetPrivateWsClient& operator=(const BitgetPrivateWsClient&) = delete;

    void start();
    void stop();

    /// Subscribe to a private channel (e.g. "orders", "positions", "account")
    void subscribe(const std::string& inst_type, const std::string& channel);

    bool is_connected() const;
    bool is_authenticated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tb::exchange::bitget
