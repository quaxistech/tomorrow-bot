#pragma once
#include "normalized_events.hpp"
#include "exchange/bitget/bitget_models.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#include <atomic>

namespace tb::normalizer {

// Нормализатор данных Bitget → внутренние события
class BitgetNormalizer {
public:
    explicit BitgetNormalizer(NormalizedEventCallback callback,
                              std::shared_ptr<tb::clock::IClock> clock,
                              std::shared_ptr<tb::logging::ILogger> logger);

    // Разбирает входящее WebSocket сообщение от Bitget
    // Безопасно обрабатывает битые/неполные JSON
    void process_raw_message(const exchange::bitget::RawWsMessage& msg);

    // Устанавливает наблюдаемые символы (для фильтрации)
    void set_symbols(std::vector<tb::Symbol> symbols);

private:
    std::optional<NormalizedTicker>    parse_ticker(const exchange::bitget::RawTicker& raw, int64_t received_ns);
    std::optional<NormalizedTrade>     parse_trade(const exchange::bitget::RawTrade& raw, int64_t received_ns);
    std::optional<NormalizedOrderBook> parse_order_book(const exchange::bitget::RawOrderBook& raw, int64_t received_ns);
    std::optional<NormalizedCandle>    parse_candle(const exchange::bitget::RawCandle& raw, int64_t received_ns);

    NormalizedEventCallback callback_;
    std::shared_ptr<tb::clock::IClock> clock_;
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::vector<tb::Symbol> symbols_;
    mutable std::mutex symbols_mutex_;   ///< Protects symbols_ from concurrent access
    std::atomic<uint64_t> sequence_{0};
};

} // namespace tb::normalizer
