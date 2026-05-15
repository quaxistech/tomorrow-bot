#pragma once
#include "features/feature_engine.hpp"
#include "features/feature_snapshot.hpp"
#include "order_book/order_book.hpp"
#include "normalizer/normalizer.hpp"
#include "exchange/bitget/bitget_ws_client.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <functional>
#include <atomic>
#include <vector>
#include <string>

namespace tb::market_data {

using FeatureSnapshotCallback = std::function<void(features::FeatureSnapshot)>;
using TradeCallback = std::function<void(double price, double volume, bool is_buy)>;

// Конфигурация шлюза рыночных данных (USDT-M фьючерсы)
struct GatewayConfig {
    exchange::bitget::WsClientConfig ws_config;
    std::vector<tb::Symbol> symbols;
    std::vector<std::string> intervals{"1m", "5m", "1h"};
    std::string inst_type{"USDT-FUTURES"};  ///< Bitget instrument type (USDT-M futures only)
    bool subscribe_tickers{true};
    bool subscribe_trades{true};
    bool subscribe_order_book{true};
    bool subscribe_candles{true};
};

// Шлюз рыночных данных — верхний уровень пайплайна
// Соединяет: WsClient → Normalizer → OrderBook + FeatureEngine
// НЕ содержит логику стратегий или исполнения ордеров
class MarketDataGateway {
public:
    MarketDataGateway(
        GatewayConfig config,
        std::shared_ptr<features::FeatureEngine> feature_engine,
        std::shared_ptr<order_book::LocalOrderBook> order_book,
        std::shared_ptr<tb::logging::ILogger> logger,
        std::shared_ptr<tb::metrics::IMetricsRegistry> metrics,
        std::shared_ptr<tb::clock::IClock> clock,
        FeatureSnapshotCallback on_snapshot = nullptr,
        TradeCallback on_trade = nullptr
    );
    ~MarketDataGateway();

    // Некопируемый
    MarketDataGateway(const MarketDataGateway&)            = delete;
    MarketDataGateway& operator=(const MarketDataGateway&) = delete;

    void start();
    void stop();

    bool is_connected() const;
    bool is_feed_fresh() const;

private:
    void on_raw_message(exchange::bitget::RawWsMessage msg);
    void on_connection_changed(bool connected);
    void on_normalized_event(normalizer::NormalizedEvent event);
    void subscribe_to_channels();

    GatewayConfig config_;
    std::shared_ptr<features::FeatureEngine> feature_engine_;
    std::shared_ptr<order_book::LocalOrderBook> order_book_;
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::shared_ptr<tb::metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<tb::clock::IClock> clock_;
    FeatureSnapshotCallback on_snapshot_;
    TradeCallback on_trade_;

    std::unique_ptr<exchange::bitget::BitgetWsClient> ws_client_;
    std::unique_ptr<normalizer::BitgetNormalizer> normalizer_;

    std::atomic<bool> running_{false};
    std::atomic<int64_t> last_message_ts_{0};
    std::atomic<uint64_t> raw_message_count_{0}; ///< Счётчик полученных рыночных сообщений
};

} // namespace tb::market_data
