#include "normalizer/normalizer.hpp"
#include <boost/json.hpp>
#include <stdexcept>
#include <charconv>
#include <algorithm>

namespace tb::normalizer {

namespace json = boost::json;

// Безопасно извлекает строку из JSON-значения (возвращает пустую строку при ошибке)
static std::string safe_string(const json::value* v) {
    if (!v) return {};
    if (auto* s = v->if_string()) return std::string(s->begin(), s->end());
    return {};
}

// Безопасно извлекает строку из JSON-объекта по ключу
static std::string safe_string(const json::object& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end()) return {};
    return safe_string(&it->value());
}

// Преобразует строку в double, возвращает 0.0 при ошибке
static double to_double(const std::string& s) {
    if (s.empty()) return 0.0;
    try {
        return std::stod(s);
    } catch (...) {
        return 0.0;
    }
}

// Преобразует строку в int64_t, возвращает 0 при ошибке
static int64_t to_int64(const std::string& s) {
    if (s.empty()) return 0;
    int64_t result = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    return (ec == std::errc{}) ? result : 0;
}

BitgetNormalizer::BitgetNormalizer(NormalizedEventCallback callback,
                                   std::shared_ptr<tb::clock::IClock> clock,
                                   std::shared_ptr<tb::logging::ILogger> logger)
    : callback_(std::move(callback))
    , clock_(std::move(clock))
    , logger_(std::move(logger))
{}

void BitgetNormalizer::set_symbols(std::vector<tb::Symbol> symbols) {
    symbols_ = std::move(symbols);
}

void BitgetNormalizer::process_raw_message(const exchange::bitget::RawWsMessage& msg) {
    if (msg.raw_payload.empty()) return;

    json::value root;
    try {
        root = json::parse(msg.raw_payload);
    } catch (const std::exception& ex) {
        logger_->warn("BitgetNormalizer", "Ошибка разбора JSON",
                      {{"error", ex.what()}, {"payload_prefix", msg.raw_payload.substr(0, 64)}});
        return;
    } catch (...) {
        logger_->warn("BitgetNormalizer", "Неизвестная ошибка разбора JSON");
        return;
    }

    const auto* root_obj = root.if_object();
    if (!root_obj) return;

    // Извлекаем arg.channel и arg.instId
    const json::object* arg_obj = nullptr;
    {
        auto arg_it = root_obj->find("arg");
        if (arg_it != root_obj->end()) {
            arg_obj = arg_it->value().if_object();
        }
    }
    if (!arg_obj) return;

    std::string channel  = safe_string(*arg_obj, "channel");
    std::string inst_id  = safe_string(*arg_obj, "instId");
    std::string action   = safe_string(*root_obj, "action");

    // Фильтрация по символам, если список задан
    if (!symbols_.empty() && !inst_id.empty()) {
        tb::Symbol sym{inst_id};
        bool found = std::ranges::find(symbols_, sym) != symbols_.end();
        if (!found) return;
    }

    // Извлекаем массив data
    const json::array* data_arr = nullptr;
    {
        auto data_it = root_obj->find("data");
        if (data_it != root_obj->end()) {
            data_arr = data_it->value().if_array();
        }
    }
    if (!data_arr || data_arr->empty()) return;

    // Определяем тип канала и разбираем
    if (channel == "ticker") {
        for (const auto& item : *data_arr) {
            const auto* obj = item.if_object();
            if (!obj) continue;

            exchange::bitget::RawTicker raw;
            raw.symbol     = inst_id.empty() ? safe_string(*obj, "instId") : inst_id;
            raw.last       = safe_string(*obj, "lastPr");
            raw.bid        = safe_string(*obj, "bidPr");
            raw.ask        = safe_string(*obj, "askPr");
            raw.volume_24h = safe_string(*obj, "baseVolume");
            raw.change_24h = safe_string(*obj, "change24h");
            raw.ts         = to_int64(safe_string(*obj, "ts"));

            auto result = parse_ticker(raw, msg.received_ns);
            if (result) callback_(std::move(*result));
        }
    } else if (channel == "trade") {
        for (const auto& item : *data_arr) {
            const auto* obj = item.if_object();
            if (!obj) continue;

            exchange::bitget::RawTrade raw;
            raw.symbol   = inst_id;
            raw.trade_id = safe_string(*obj, "tradeId");
            raw.price    = safe_string(*obj, "price");
            raw.size     = safe_string(*obj, "size");
            raw.side     = safe_string(*obj, "side");
            raw.ts       = to_int64(safe_string(*obj, "ts"));

            auto result = parse_trade(raw, msg.received_ns);
            if (result) callback_(std::move(*result));
        }
    } else if (channel.starts_with("books")) {
        for (const auto& item : *data_arr) {
            const auto* obj = item.if_object();
            if (!obj) continue;

            exchange::bitget::RawOrderBook raw;
            raw.symbol   = inst_id;
            raw.action   = action;
            raw.ts       = to_int64(safe_string(*obj, "ts"));

            // Разбираем seqId
            {
                auto seq_it = obj->find("seqId");
                if (seq_it != obj->end()) {
                    if (auto* n = seq_it->value().if_int64())
                        raw.sequence = static_cast<uint64_t>(*n);
                    else if (auto* u = seq_it->value().if_uint64())
                        raw.sequence = *u;
                    else
                        raw.sequence = to_int64(safe_string(*obj, "seqId"));
                }
            }

            // Разбираем bids/asks: каждый уровень — массив [цена, размер]
            auto parse_levels = [](const json::array* levels_arr,
                                   std::vector<exchange::bitget::RawBookLevel>& out) {
                if (!levels_arr) return;
                out.reserve(levels_arr->size());
                for (const auto& lvl : *levels_arr) {
                    const auto* pair = lvl.if_array();
                    if (!pair || pair->size() < 2) continue;
                    exchange::bitget::RawBookLevel level;
                    level.price = safe_string(&(*pair)[0]);
                    level.size  = safe_string(&(*pair)[1]);
                    out.push_back(std::move(level));
                }
            };

            {
                auto bids_it = obj->find("bids");
                if (bids_it != obj->end()) parse_levels(bids_it->value().if_array(), raw.bids);
                auto asks_it = obj->find("asks");
                if (asks_it != obj->end()) parse_levels(asks_it->value().if_array(), raw.asks);
            }

            auto result = parse_order_book(raw, msg.received_ns);
            if (result) callback_(std::move(*result));
        }
    } else if (channel.starts_with("candle")) {
        // Свечи приходят как массив массивов: [ts, open, high, low, close, volume, base_volume, ?is_closed?]
        std::string interval = channel.substr(6); // убираем "candle" префикс

        // Длительность интервала в миллисекундах для определения закрытости свечи
        int64_t interval_ms = 60'000; // 1m по умолчанию
        if (interval == "5m") interval_ms = 300'000;
        else if (interval == "15m") interval_ms = 900'000;
        else if (interval == "1H") interval_ms = 3'600'000;

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& item : *data_arr) {
            const auto* arr = item.if_array();
            if (!arr || arr->size() < 7) continue;

            exchange::bitget::RawCandle raw;
            raw.symbol      = inst_id;
            raw.interval    = interval;
            raw.ts          = to_int64(safe_string(&(*arr)[0]));
            raw.open        = safe_string(&(*arr)[1]);
            raw.high        = safe_string(&(*arr)[2]);
            raw.low         = safe_string(&(*arr)[3]);
            raw.close       = safe_string(&(*arr)[4]);
            raw.volume      = safe_string(&(*arr)[5]);
            raw.base_volume = safe_string(&(*arr)[6]);

            // is_closed определяется тремя способами:
            // 1. Явно задан 8-й элемент "1"
            // 2. Период свечи уже истёк (ts + interval < now)
            // 3. Если ни то ни другое — свеча считается открытой
            if (arr->size() > 7 && safe_string(&(*arr)[7]) == "1") {
                raw.is_closed = true;
            } else {
                raw.is_closed = (raw.ts + interval_ms <= now_ms);
            }

            auto result = parse_candle(raw, msg.received_ns);
            if (result) callback_(std::move(*result));
        }
    } else {
        logger_->debug("BitgetNormalizer", "Неизвестный канал",
                       {{"channel", channel}, {"inst_id", inst_id}});
    }
}

std::optional<NormalizedTicker> BitgetNormalizer::parse_ticker(
    const exchange::bitget::RawTicker& raw, int64_t received_ns)
{
    NormalizedTicker ticker;
    ticker.envelope.symbol       = tb::Symbol{raw.symbol};
    ticker.envelope.exchange_ts  = tb::Timestamp{raw.ts};
    ticker.envelope.received_ts  = tb::Timestamp{received_ns};
    ticker.envelope.processed_ts = clock_->now();
    ticker.envelope.source       = "bitget";
    ticker.envelope.sequence_id  = ++sequence_;

    ticker.last_price   = tb::Price{to_double(raw.last)};
    ticker.bid          = tb::Price{to_double(raw.bid)};
    ticker.ask          = tb::Price{to_double(raw.ask)};
    ticker.volume_24h   = tb::Quantity{to_double(raw.volume_24h)};
    ticker.change_24h_pct = to_double(raw.change_24h);

    const double bid_val = ticker.bid.get();
    const double ask_val = ticker.ask.get();
    if (ask_val > 0.0 && bid_val > 0.0) {
        ticker.spread     = ask_val - bid_val;
        const double mid  = (ask_val + bid_val) * 0.5;
        ticker.spread_bps = (mid > 0.0) ? (ticker.spread / mid) * 10000.0 : 0.0;
    }

    return ticker;
}

std::optional<NormalizedTrade> BitgetNormalizer::parse_trade(
    const exchange::bitget::RawTrade& raw, int64_t received_ns)
{
    NormalizedTrade trade;
    trade.envelope.symbol       = tb::Symbol{raw.symbol};
    trade.envelope.exchange_ts  = tb::Timestamp{raw.ts};
    trade.envelope.received_ts  = tb::Timestamp{received_ns};
    trade.envelope.processed_ts = clock_->now();
    trade.envelope.source       = "bitget";
    trade.envelope.sequence_id  = ++sequence_;

    trade.trade_id = raw.trade_id;
    trade.price    = tb::Price{to_double(raw.price)};
    trade.size     = tb::Quantity{to_double(raw.size)};
    trade.side     = (raw.side == "sell") ? tb::Side::Sell : tb::Side::Buy;
    // Агрессор определяется по стороне: в Bitget маркет-тейкер всегда агрессор
    trade.is_aggressive = true;

    return trade;
}

std::optional<NormalizedOrderBook> BitgetNormalizer::parse_order_book(
    const exchange::bitget::RawOrderBook& raw, int64_t received_ns)
{
    NormalizedOrderBook book;
    book.envelope.symbol       = tb::Symbol{raw.symbol};
    book.envelope.exchange_ts  = tb::Timestamp{raw.ts};
    book.envelope.received_ts  = tb::Timestamp{received_ns};
    book.envelope.processed_ts = clock_->now();
    book.envelope.source       = "bitget";
    book.envelope.sequence_id  = ++sequence_;

    book.update_type = (raw.action == "snapshot")
                       ? BookUpdateType::Snapshot
                       : BookUpdateType::Delta;
    book.sequence    = raw.sequence;

    book.bids.reserve(raw.bids.size());
    for (const auto& lvl : raw.bids) {
        book.bids.push_back({tb::Price{to_double(lvl.price)},
                             tb::Quantity{to_double(lvl.size)}});
    }

    book.asks.reserve(raw.asks.size());
    for (const auto& lvl : raw.asks) {
        book.asks.push_back({tb::Price{to_double(lvl.price)},
                             tb::Quantity{to_double(lvl.size)}});
    }

    return book;
}

std::optional<NormalizedCandle> BitgetNormalizer::parse_candle(
    const exchange::bitget::RawCandle& raw, int64_t received_ns)
{
    NormalizedCandle candle;
    candle.envelope.symbol       = tb::Symbol{raw.symbol};
    candle.envelope.exchange_ts  = tb::Timestamp{raw.ts};
    candle.envelope.received_ts  = tb::Timestamp{received_ns};
    candle.envelope.processed_ts = clock_->now();
    candle.envelope.source       = "bitget";
    candle.envelope.sequence_id  = ++sequence_;

    candle.interval    = raw.interval;
    candle.open        = tb::Price{to_double(raw.open)};
    candle.high        = tb::Price{to_double(raw.high)};
    candle.low         = tb::Price{to_double(raw.low)};
    candle.close       = tb::Price{to_double(raw.close)};
    candle.volume      = tb::Quantity{to_double(raw.volume)};
    candle.base_volume = tb::Quantity{to_double(raw.base_volume)};
    candle.is_closed   = raw.is_closed;

    return candle;
}

} // namespace tb::normalizer
