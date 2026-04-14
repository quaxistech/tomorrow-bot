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
#include <cstdint>

namespace tb::normalizer {

/// Нормализатор рыночных данных Bitget USDT-M Futures → внутренние типы.
///
/// Принимает сырой WebSocket JSON, парсит payload, валидирует данные
/// и генерирует строго типизированные NormalizedEvent для downstream pipeline.
/// Все временны́е метки приведены к наносекундам от Unix epoch.
class BitgetNormalizer {
public:
    explicit BitgetNormalizer(NormalizedEventCallback callback,
                              std::shared_ptr<tb::clock::IClock> clock,
                              std::shared_ptr<tb::logging::ILogger> logger);

    /// Разбирает входящее WebSocket-сообщение от Bitget.
    /// Безопасно обрабатывает битые/неполные JSON.
    /// Фильтрует non-futures сообщения.
    void process_raw_message(const exchange::bitget::RawWsMessage& msg);

    /// Устанавливает наблюдаемые символы (для фильтрации).
    void set_symbols(std::vector<tb::Symbol> symbols);

    /// Количество отклонённых событий из-за невалидных данных (для мониторинга).
    [[nodiscard]] uint64_t rejected_count() const noexcept { return rejected_count_.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] std::optional<NormalizedTicker>    parse_ticker(const exchange::bitget::RawTicker& raw, int64_t received_ns);
    [[nodiscard]] std::optional<NormalizedTrade>     parse_trade(const exchange::bitget::RawTrade& raw, int64_t received_ns);
    [[nodiscard]] std::optional<NormalizedOrderBook> parse_order_book(const exchange::bitget::RawOrderBook& raw, int64_t received_ns);
    [[nodiscard]] std::optional<NormalizedCandle>    parse_candle(const exchange::bitget::RawCandle& raw, int64_t received_ns);

    /// Заполняет общий envelope для любого события.
    void fill_envelope(EventEnvelope& env, const std::string& symbol, int64_t exchange_ts_ms, int64_t received_ns);

    NormalizedEventCallback callback_;
    std::shared_ptr<tb::clock::IClock> clock_;
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::vector<tb::Symbol> symbols_;
    mutable std::mutex symbols_mutex_;
    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> rejected_count_{0};
};

} // namespace tb::normalizer
