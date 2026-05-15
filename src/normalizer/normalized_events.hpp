#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <variant>
#include <functional>

namespace tb::normalizer {

/// Единый конверт события — метаданные для replay и observe
struct EventEnvelope {
    tb::Timestamp exchange_ts{0};   ///< Время биржи, наносекунды от Unix epoch
    tb::Timestamp received_ts{0};   ///< Время локального получения, наносекунды
    tb::Timestamp processed_ts{0};  ///< Время нормализации, наносекунды
    tb::Symbol symbol{""};          ///< Инструмент (USDT-M futures symbol, e.g. "BTCUSDT")
    std::string source;             ///< Источник данных ("bitget")
    uint64_t sequence_id{0};        ///< Монотонный счётчик порядка обработки
};

/// Нормализованный тикер (top-of-book snapshot)
struct NormalizedTicker {
    EventEnvelope envelope;
    tb::Price last_price{0.0};
    tb::Price bid{0.0};            ///< Лучший bid
    tb::Price ask{0.0};            ///< Лучший ask
    tb::Quantity volume_24h{0.0};  ///< 24h объём в базовой валюте
    double change_24h_pct{0.0};    ///< 24h изменение — raw-значение с биржи (ratio, e.g. 0.02 = 2%)
    double spread{0.0};            ///< ask - bid в единицах цены
    double spread_bps{0.0};        ///< Спред в базисных пунктах (bps = spread / mid * 10000)
};

/// Нормализованная сделка (tape print)
struct NormalizedTrade {
    EventEnvelope envelope;
    std::string trade_id;
    tb::Price price{0.0};
    tb::Quantity size{0.0};
    tb::Side side{tb::Side::Buy};  ///< Направление тейкера: Buy = покупатель-тейкер, Sell = продавец-тейкер
    bool is_aggressive{false};     ///< Sell-side taker: true = продавец бьёт по бидам (sell pressure)
};

/// Один уровень нормализованного стакана
struct BookLevel {
    tb::Price price{0.0};
    tb::Quantity size{0.0};
};

/// Тип обновления стакана
enum class BookUpdateType { Snapshot, Delta };

/// Нормализованное обновление стакана (USDT-M futures order book)
struct NormalizedOrderBook {
    EventEnvelope envelope;
    BookUpdateType update_type{BookUpdateType::Snapshot};
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    uint64_t sequence{0};   ///< Биржевой seqId для контроля пропусков
};

/// Нормализованная свеча (OHLCV)
struct NormalizedCandle {
    EventEnvelope envelope;
    std::string interval;       ///< Интервал (e.g. "1m", "5m", "1h")
    tb::Price open{0.0}, high{0.0}, low{0.0}, close{0.0};
    tb::Quantity volume{0.0};       ///< Объём в quote currency
    tb::Quantity base_volume{0.0};  ///< Объём в base currency
    bool is_closed{false};
};

/// Вариантный тип для всех нормализованных событий
using NormalizedEvent = std::variant<
    NormalizedTicker,
    NormalizedTrade,
    NormalizedOrderBook,
    NormalizedCandle
>;

/// Callback на нормализованное событие
using NormalizedEventCallback = std::function<void(NormalizedEvent)>;

} // namespace tb::normalizer
