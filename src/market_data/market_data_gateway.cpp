#include "market_data_gateway.hpp"
#include "health/readiness_state.hpp"
#include <variant>

namespace tb::market_data {

MarketDataGateway::MarketDataGateway(
    GatewayConfig config,
    std::shared_ptr<features::FeatureEngine> feature_engine,
    std::shared_ptr<order_book::LocalOrderBook> order_book,
    std::shared_ptr<tb::health::IHealthService> health,
    std::shared_ptr<tb::logging::ILogger> logger,
    std::shared_ptr<tb::metrics::IMetricsRegistry> metrics,
    std::shared_ptr<tb::clock::IClock> clock,
    FeatureSnapshotCallback on_snapshot)
    : config_(std::move(config))
    , feature_engine_(std::move(feature_engine))
    , order_book_(std::move(order_book))
    , health_(std::move(health))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , clock_(std::move(clock))
    , on_snapshot_(std::move(on_snapshot))
{
    // Создаём нормализатор с колбэком на нормализованные события
    normalizer_ = std::make_unique<normalizer::BitgetNormalizer>(
        [this](normalizer::NormalizedEvent e) { on_normalized_event(std::move(e)); },
        clock_,
        logger_
    );
    normalizer_->set_symbols(config_.symbols);

    // Создаём WebSocket клиент
    ws_client_ = std::make_unique<exchange::bitget::BitgetWsClient>(
        config_.ws_config,
        [this](exchange::bitget::RawWsMessage m) { on_raw_message(std::move(m)); },
        [this](bool c) { on_connection_changed(c); },
        logger_
    );

    health_->register_subsystem("market_data");
    health_->update_subsystem("market_data", tb::health::SubsystemState::Starting,
                              "Инициализация шлюза рыночных данных");
}

MarketDataGateway::~MarketDataGateway() {
    if (running_.load()) {
        stop();
    }
}

void MarketDataGateway::start() {
    running_.store(true);
    logger_->info("MarketDataGateway", "Запуск шлюза рыночных данных");
    ws_client_->start();
    subscribe_to_channels();
}

void MarketDataGateway::stop() {
    // Защита от повторного вызова
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    logger_->info("MarketDataGateway", "Остановка шлюза рыночных данных");
    ws_client_->stop();
    health_->update_subsystem("market_data", tb::health::SubsystemState::Unknown,
                              "Шлюз остановлен");
}

bool MarketDataGateway::is_connected() const {
    return ws_client_ && ws_client_->is_connected();
}

bool MarketDataGateway::is_feed_fresh() const {
    return (clock_->now().get() - last_message_ts_.load()) < 1'000'000'000LL;
}

void MarketDataGateway::on_raw_message(exchange::bitget::RawWsMessage msg) {
    last_message_ts_.store(clock_->now().get());

    // Пропускаем heartbeat-сообщения — они не содержат рыночных данных
    if (msg.type == exchange::bitget::WsMsgType::Heartbeat) return;
    // Пропускаем подтверждения подписок
    if (msg.type == exchange::bitget::WsMsgType::Subscribe) return;

    ++raw_message_count_;
    // Периодически логируем количество обработанных сообщений
    if (raw_message_count_ % 500 == 1) {
        logger_->info("MarketDataGateway", "Получено рыночных сообщений: "
                      + std::to_string(raw_message_count_));
    }

    normalizer_->process_raw_message(msg);
}

void MarketDataGateway::on_connection_changed(bool connected) {
    if (connected) {
        logger_->info("MarketDataGateway", "WebSocket подключён");
        health_->update_subsystem("market_data", tb::health::SubsystemState::Healthy,
                                  "WebSocket подключён");
        // Повторная подписка не нужна: on_ws_handshake уже отправляет
        // все сохранённые подписки из вектора subscriptions при каждом подключении
    } else {
        logger_->warn("MarketDataGateway", "WebSocket отключён, ожидаем переподключения");
        health_->update_subsystem("market_data", tb::health::SubsystemState::Degraded,
                                  "WebSocket отключён");
    }
}

void MarketDataGateway::on_normalized_event(normalizer::NormalizedEvent event) {
    std::visit([this](auto&& ev) {
        using T = std::decay_t<decltype(ev)>;

        if constexpr (std::is_same_v<T, normalizer::NormalizedOrderBook>) {
            // Применяем снимок или дельту к локальному стакану
            if (ev.update_type == normalizer::BookUpdateType::Snapshot) {
                order_book_->apply_snapshot(ev);
            } else {
                order_book_->apply_delta(ev);
            }
        } else if constexpr (std::is_same_v<T, normalizer::NormalizedTicker>) {
            feature_engine_->on_ticker(ev);
            
            // После обновления тикера пересчитываем снимок признаков
            if (on_snapshot_) {
                auto snap = feature_engine_->compute_snapshot(
                    ev.envelope.symbol, *order_book_
                );
                if (snap.has_value()) {
                    on_snapshot_(std::move(*snap));
                }
            }
        } else if constexpr (std::is_same_v<T, normalizer::NormalizedTrade>) {
            feature_engine_->on_trade(ev);
        } else if constexpr (std::is_same_v<T, normalizer::NormalizedCandle>) {
            feature_engine_->on_candle(ev);
        }
    }, std::move(event));
}

void MarketDataGateway::subscribe_to_channels() {
    for (const auto& symbol : config_.symbols) {
        const std::string inst = symbol.get();

        // Bitget v2 API: подписка требует JSON-объекты {instType, channel, instId}
        const auto& inst_type = config_.inst_type;
        auto make_sub = [&](const std::string& channel) -> std::string {
            return R"({"instType":")" + inst_type + R"(","channel":")" + channel +
                   R"(","instId":")" + inst + R"("})";
        };

        if (config_.subscribe_tickers) {
            ws_client_->subscribe(make_sub("ticker"));
        }
        if (config_.subscribe_trades) {
            ws_client_->subscribe(make_sub("trade"));
        }
        if (config_.subscribe_order_book) {
            ws_client_->subscribe(make_sub("books15"));
        }
        if (config_.subscribe_candles) {
            for (const auto& interval : config_.intervals) {
                ws_client_->subscribe(make_sub("candle" + interval));
            }
        }
    }
    logger_->info("MarketDataGateway", "Подписки отправлены для "
                  + std::to_string(config_.symbols.size()) + " символов");
}

} // namespace tb::market_data
