#pragma once
#include "common/types.hpp"
#include "exchange/bitget/bitget_models.hpp"
#include <string>
#include <vector>
#include <variant>
#include <functional>

namespace tb::normalizer {

// Базовый конверт события — содержит метаданные для replay
struct EventEnvelope {
    tb::Timestamp exchange_ts{0};
    tb::Timestamp received_ts{0};
    tb::Timestamp processed_ts{0};
    tb::Symbol symbol{""};
    std::string source;
    uint64_t sequence_id{0};
};

// Нормализованный тикер
struct NormalizedTicker {
    EventEnvelope envelope;
    tb::Price last_price{0.0};
    tb::Price bid{0.0};
    tb::Price ask{0.0};
    tb::Quantity volume_24h{0.0};
    double change_24h_pct{0.0};
    double spread{0.0};
    double spread_bps{0.0};
};

// Нормализованная сделка
struct NormalizedTrade {
    EventEnvelope envelope;
    std::string trade_id;
    tb::Price price{0.0};
    tb::Quantity size{0.0};
    tb::Side side{tb::Side::Buy};
    bool is_aggressive{false};
};

// Один уровень нормализованного стакана
struct BookLevel {
    tb::Price price{0.0};
    tb::Quantity size{0.0};
};

// Тип обновления стакана
enum class BookUpdateType { Snapshot, Delta };

// Нормализованное обновление стакана
struct NormalizedOrderBook {
    EventEnvelope envelope;
    BookUpdateType update_type{BookUpdateType::Snapshot};
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    uint64_t sequence{0};
};

// Нормализованная свеча
struct NormalizedCandle {
    EventEnvelope envelope;
    std::string interval;
    tb::Price open{0.0}, high{0.0}, low{0.0}, close{0.0};
    tb::Quantity volume{0.0};
    tb::Quantity base_volume{0.0};
    bool is_closed{false};
};

// Вариантный тип для всех нормализованных событий
using NormalizedEvent = std::variant<
    NormalizedTicker,
    NormalizedTrade,
    NormalizedOrderBook,
    NormalizedCandle
>;

// Колбэк на нормализованное событие
using NormalizedEventCallback = std::function<void(NormalizedEvent)>;

} // namespace tb::normalizer
