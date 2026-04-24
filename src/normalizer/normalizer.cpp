#include "normalizer/normalizer.hpp"
#include <boost/json.hpp>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace tb::normalizer {

namespace json = boost::json;

// ============================================================
// Вспомогательные функции (file-local)
// ============================================================

// Безопасно извлекает строку из JSON-значения
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

// Преобразует строку в double (locale-independent через from_chars)
static double to_double(const std::string& s) {
    if (s.empty()) return 0.0;
    double result = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    return (ec == std::errc{}) ? result : 0.0;
}

// Преобразует строку в int64_t
static int64_t to_int64(const std::string& s) {
    if (s.empty()) return 0;
    int64_t result = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    return (ec == std::errc{}) ? result : 0;
}

// Case-insensitive сравнение строк (ASCII-safe для биржевых полей)
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Миллисекунды → наносекунды (Bitget ts → internal Timestamp)
static constexpr int64_t ms_to_ns(int64_t ms) {
    return ms * 1'000'000LL;
}

// Bitget USDT-M Futures: все поддерживаемые интервалы свечей → длительность в ms.
// Ref: Bitget API v2 /api/v2/mix/market/candles — granularity параметр.
static int64_t interval_to_ms(std::string_view interval) {
    // Стандартные интервалы USDT-M Futures (Bitget v2 API)
    if (interval == "1m")                       return       60'000LL;
    if (interval == "3m")                       return      180'000LL;
    if (interval == "5m")                       return      300'000LL;
    if (interval == "15m")                      return      900'000LL;
    if (interval == "30m")                      return    1'800'000LL;
    if (interval == "1h"  || interval == "1H")  return    3'600'000LL;
    if (interval == "2h"  || interval == "2H")  return    7'200'000LL;
    if (interval == "4h"  || interval == "4H")  return   14'400'000LL;
    if (interval == "6h"  || interval == "6H"  ||
        interval == "6Hutc")                    return   21'600'000LL;
    if (interval == "12h" || interval == "12H" ||
        interval == "12Hutc")                   return   43'200'000LL;
    if (interval == "1d"  || interval == "1D"  ||
        interval == "1Dutc")                    return   86'400'000LL;
    if (interval == "3d"  || interval == "3D"  ||
        interval == "3Dutc")                    return  259'200'000LL;
    if (interval == "1w"  || interval == "1W"  ||
        interval == "1Wutc")                    return  604'800'000LL;
    if (interval == "1M"  || interval == "1Mutc")
                                                return 2'592'000'000LL; // ~30 дней
    return 0; // неизвестный интервал
}

// ============================================================
// Конструктор
// ============================================================

BitgetNormalizer::BitgetNormalizer(NormalizedEventCallback callback,
                                   std::shared_ptr<tb::clock::IClock> clock,
                                   std::shared_ptr<tb::logging::ILogger> logger)
    : callback_(std::move(callback))
    , clock_(std::move(clock))
    , logger_(std::move(logger))
{}

// ============================================================
// set_symbols
// ============================================================

void BitgetNormalizer::set_symbols(std::vector<tb::Symbol> symbols) {
    std::lock_guard<std::mutex> lock(symbols_mutex_);
    symbols_ = std::move(symbols);
}

// ============================================================
// fill_envelope — единая точка заполнения метаданных
// ============================================================

void BitgetNormalizer::fill_envelope(EventEnvelope& env, const std::string& symbol,
                                     int64_t exchange_ts_ms, int64_t received_ns) {
    env.symbol       = tb::Symbol{symbol};
    env.exchange_ts  = tb::Timestamp{ms_to_ns(exchange_ts_ms)};
    env.received_ts  = tb::Timestamp{received_ns};
    env.processed_ts = clock_->now();
    env.source       = "bitget";
    env.sequence_id  = ++sequence_;
}

// ============================================================
// process_raw_message — центральный dispatch
// ============================================================

void BitgetNormalizer::process_raw_message(const exchange::bitget::RawWsMessage& msg) {
    if (msg.raw_payload.empty()) return;

    // 1. Безопасный JSON-парсинг
    json::value root;
    try {
        root = json::parse(msg.raw_payload);
    } catch (const std::exception& ex) {
        logger_->warn("BitgetNormalizer", "JSON parse error",
                      {{"error", ex.what()},
                       {"payload_prefix", msg.raw_payload.substr(0, 64)}});
        return;
    } catch (...) {
        logger_->warn("BitgetNormalizer", "Unknown JSON parse error");
        return;
    }

    const auto* root_obj = root.if_object();
    if (!root_obj) return;

    // 2. Извлекаем arg — обязательный объект
    const json::object* arg_obj = nullptr;
    {
        auto arg_it = root_obj->find("arg");
        if (arg_it != root_obj->end()) {
            arg_obj = arg_it->value().if_object();
        }
    }
    if (!arg_obj) return;

    const std::string channel  = safe_string(*arg_obj, "channel");
    const std::string inst_id  = safe_string(*arg_obj, "instId");

    // 3. Фьючерсный фильтр: отклоняем non-USDT-M сообщения.
    // Bitget v2 API включает instType в arg для каждого сообщения.
    // Допустимые значения: "USDT-FUTURES", "mc" (legacy), "umcbl" (legacy).
    {
        const std::string inst_type = safe_string(*arg_obj, "instType");
        if (!inst_type.empty() &&
            !iequals(inst_type, "USDT-FUTURES") &&
            !iequals(inst_type, "mc") &&
            !iequals(inst_type, "umcbl")) {
            // Не USDT-M futures — отбрасываем
            return;
        }
    }

    // 4. Фильтрация по символам
    if (!inst_id.empty()) {
        std::lock_guard<std::mutex> sym_lock(symbols_mutex_);
        if (!symbols_.empty()) {
            tb::Symbol sym{inst_id};
            if (std::ranges::find(symbols_, sym) == symbols_.end()) return;
        }
    }

    // 5. Извлекаем массив data
    const json::array* data_arr = nullptr;
    {
        auto data_it = root_obj->find("data");
        if (data_it != root_obj->end()) {
            data_arr = data_it->value().if_array();
        }
    }
    if (!data_arr || data_arr->empty()) return;

    const std::string action = safe_string(*root_obj, "action");

    // 6. Dispatch по каналу
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

            if (auto result = parse_ticker(raw, msg.received_ns))
                callback_(std::move(*result));
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

            if (auto result = parse_trade(raw, msg.received_ns))
                callback_(std::move(*result));
        }
    } else if (channel.starts_with("books")) {
        for (const auto& item : *data_arr) {
            const auto* obj = item.if_object();
            if (!obj) continue;

            exchange::bitget::RawOrderBook raw;
            raw.symbol   = inst_id;
            raw.action   = action;
            raw.ts       = to_int64(safe_string(*obj, "ts"));

            // Разбираем seqId (может быть числом или строкой)
            {
                auto seq_it = obj->find("seqId");
                if (seq_it != obj->end()) {
                    if (auto* n = seq_it->value().if_int64())
                        raw.sequence = static_cast<uint64_t>(*n);
                    else if (auto* u = seq_it->value().if_uint64())
                        raw.sequence = *u;
                    else
                        raw.sequence = static_cast<uint64_t>(to_int64(safe_string(*obj, "seqId")));
                }
            }

            // Разбираем уровни: каждый — [price, size]
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

            if (auto result = parse_order_book(raw, msg.received_ns))
                callback_(std::move(*result));
        }
    } else if (channel.starts_with("candle")) {
        // Интервал из имени канала: "candle1m" → "1m"
        const std::string interval = channel.substr(6);
        int64_t interval_ms = interval_to_ms(interval);

        if (interval_ms == 0) {
            logger_->warn("BitgetNormalizer", "Unknown candle interval",
                          {{"channel", channel}, {"interval", interval}});
            // Fail-soft fallback: обрабатываем как 1m, чтобы не терять live-поток
            // при появлении нового/нестандартного алиаса интервала.
            interval_ms = 60'000;
        }

        // Время в ms для определения закрытости свечи
        // Используем инжектированный clock (replay/backtest-compatible)
        const int64_t now_ms = clock_->now().get() / 1'000'000LL;

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

            // is_closed: 3-step logic
            // 1. Явный флаг "1" в 8-м элементе (Bitget v2)
            // 2. Временна́я проверка: candle_open + interval_ms <= now
            // 3. Иначе — свеча открыта
            if (arr->size() > 7 && safe_string(&(*arr)[7]) == "1") {
                raw.is_closed = true;
            } else {
                raw.is_closed = (raw.ts + interval_ms <= now_ms);
            }

            if (auto result = parse_candle(raw, msg.received_ns))
                callback_(std::move(*result));
        }
    } else {
        logger_->debug("BitgetNormalizer", "Unknown channel",
                       {{"channel", channel}, {"inst_id", inst_id}});
    }
}

// ============================================================
// parse_ticker — нормализация тикера с валидацией
// ============================================================

std::optional<NormalizedTicker> BitgetNormalizer::parse_ticker(
    const exchange::bitget::RawTicker& raw, int64_t received_ns)
{
    NormalizedTicker ticker;
    fill_envelope(ticker.envelope, raw.symbol, raw.ts, received_ns);

    ticker.last_price   = tb::Price{to_double(raw.last)};
    ticker.bid          = tb::Price{to_double(raw.bid)};
    ticker.ask          = tb::Price{to_double(raw.ask)};
    ticker.volume_24h   = tb::Quantity{to_double(raw.volume_24h)};
    ticker.change_24h_pct = to_double(raw.change_24h);

    const double bid_val = ticker.bid.get();
    const double ask_val = ticker.ask.get();
    const double last_val = ticker.last_price.get();

    // Валидация: цены должны быть положительными
    if (last_val <= 0.0 || bid_val <= 0.0 || ask_val <= 0.0) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Ticker rejected: non-positive prices",
                      {{"symbol", raw.symbol},
                       {"last", raw.last}, {"bid", raw.bid}, {"ask", raw.ask}});
        return std::nullopt;
    }

    // Валидация: ask >= bid (crossed book = аномалия)
    if (ask_val < bid_val) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Ticker rejected: crossed book (ask < bid)",
                      {{"symbol", raw.symbol},
                       {"bid", raw.bid}, {"ask", raw.ask}});
        return std::nullopt;
    }

    // Спред и спред в bps
    ticker.spread = ask_val - bid_val;
    const double mid = (ask_val + bid_val) * 0.5;
    ticker.spread_bps = (mid > 0.0) ? (ticker.spread / mid) * 10'000.0 : 0.0;

    return ticker;
}

// ============================================================
// parse_trade — нормализация сделки с валидацией
// ============================================================

std::optional<NormalizedTrade> BitgetNormalizer::parse_trade(
    const exchange::bitget::RawTrade& raw, int64_t received_ns)
{
    const double price_val = to_double(raw.price);
    const double size_val  = to_double(raw.size);

    // Валидация: цена и размер должны быть положительными
    if (price_val <= 0.0 || size_val <= 0.0) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Trade rejected: non-positive price or size",
                      {{"symbol", raw.symbol},
                       {"price", raw.price}, {"size", raw.size}});
        return std::nullopt;
    }

    NormalizedTrade trade;
    fill_envelope(trade.envelope, raw.symbol, raw.ts, received_ns);

    trade.trade_id = raw.trade_id;
    trade.price    = tb::Price{price_val};
    trade.size     = tb::Quantity{size_val};

    // Bitget v2 USDT-M: side = "Buy"/"Sell" (capitalized) — направление тейкера.
    // Bitget v1 legacy: side = "buy"/"sell" (lowercase).
    // Обрабатываем оба варианта case-insensitively.
    const bool is_sell = iequals(raw.side, "sell");
    trade.side = is_sell ? tb::Side::Sell : tb::Side::Buy;

    // is_aggressive = sell-side taker indicator.
    // Все tape prints — это taker fills. Поле указывает направление давления:
    // true  = продавец-тейкер бьёт по бидам (sell pressure)
    // false = покупатель-тейкер поднимает аски (buy pressure)
    // Downstream: TradeBuffer::aggressive_flow() = sell_taker_vol / total_vol
    trade.is_aggressive = is_sell;

    return trade;
}

// ============================================================
// parse_order_book — нормализация стакана с валидацией
// ============================================================

std::optional<NormalizedOrderBook> BitgetNormalizer::parse_order_book(
    const exchange::bitget::RawOrderBook& raw, int64_t received_ns)
{
    NormalizedOrderBook book;
    fill_envelope(book.envelope, raw.symbol, raw.ts, received_ns);

    // Bitget USDT-M: action = "snapshot" | "update"
    if (iequals(raw.action, "snapshot")) {
        book.update_type = BookUpdateType::Snapshot;
    } else {
        book.update_type = BookUpdateType::Delta;
    }
    book.sequence = raw.sequence;

    // Конвертируем уровни, отбрасывая невалидные (price <= 0)
    book.bids.reserve(raw.bids.size());
    for (const auto& lvl : raw.bids) {
        const double p = to_double(lvl.price);
        const double s = to_double(lvl.size);
        if (p <= 0.0) continue; // невалидный уровень
        book.bids.push_back({tb::Price{p}, tb::Quantity{s}});
    }

    book.asks.reserve(raw.asks.size());
    for (const auto& lvl : raw.asks) {
        const double p = to_double(lvl.price);
        const double s = to_double(lvl.size);
        if (p <= 0.0) continue; // невалидный уровень
        book.asks.push_back({tb::Price{p}, tb::Quantity{s}});
    }

    // Crossed book validation: best_bid must be < best_ask
    if (!book.bids.empty() && !book.asks.empty()) {
        const double best_bid = book.bids.front().price.get();
        const double best_ask = book.asks.front().price.get();
        if (best_bid >= best_ask) {
            return std::nullopt;  // crossed/locked book — discard
        }
    }

    return book;
}

// ============================================================
// parse_candle — нормализация свечи с валидацией OHLC
// ============================================================

std::optional<NormalizedCandle> BitgetNormalizer::parse_candle(
    const exchange::bitget::RawCandle& raw, int64_t received_ns)
{
    const double o = to_double(raw.open);
    const double h = to_double(raw.high);
    const double l = to_double(raw.low);
    const double c = to_double(raw.close);
    const double v = to_double(raw.volume);

    // Валидация OHLC:
    // - Все цены положительные
    // - high >= low (физическое ограничение)
    // - high >= open && high >= close
    // - low  <= open && low  <= close
    if (o <= 0.0 || h <= 0.0 || l <= 0.0 || c <= 0.0) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Candle rejected: non-positive OHLC",
                      {{"symbol", raw.symbol}, {"interval", raw.interval},
                       {"o", raw.open}, {"h", raw.high}, {"l", raw.low}, {"c", raw.close}});
        return std::nullopt;
    }
    if (h < l) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Candle rejected: high < low",
                      {{"symbol", raw.symbol}, {"interval", raw.interval},
                       {"high", raw.high}, {"low", raw.low}});
        return std::nullopt;
    }
    if (h < o || h < c || l > o || l > c) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Candle rejected: OHLC inconsistency",
                      {{"symbol", raw.symbol}, {"interval", raw.interval},
                       {"o", raw.open}, {"h", raw.high}, {"l", raw.low}, {"c", raw.close}});
        return std::nullopt;
    }
    if (v < 0.0) {
        ++rejected_count_;
        logger_->warn("BitgetNormalizer", "Candle rejected: negative volume",
                      {{"symbol", raw.symbol}, {"volume", raw.volume}});
        return std::nullopt;
    }

    NormalizedCandle candle;
    fill_envelope(candle.envelope, raw.symbol, raw.ts, received_ns);

    candle.interval    = raw.interval;
    candle.open        = tb::Price{o};
    candle.high        = tb::Price{h};
    candle.low         = tb::Price{l};
    candle.close       = tb::Price{c};
    candle.volume      = tb::Quantity{v};
    candle.base_volume = tb::Quantity{to_double(raw.base_volume)};
    candle.is_closed   = raw.is_closed;

    return candle;
}

} // namespace tb::normalizer
