/**
 * @file normalizer_test.cpp
 * @brief Комплексные unit-тесты модуля BitgetNormalizer
 *
 * Покрытие:
 * - Все 4 канала: ticker, trade, books*, candle*
 * - Валидация данных (отклонение невалидных цен/объёмов)
 * - Фильтрация по символам
 * - Фильтрация по instType (только USDT-FUTURES)
 * - Корректность exchange_ts (ms → ns конверсия)
 * - Case-insensitive side matching (Bitget v1/v2)
 * - Candle closure logic
 * - Семантика spread / spread_bps
 * - Order book snapshot vs delta
 * - Робастность на edge cases
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "normalizer/normalizer.hpp"
#include "exchange/bitget/bitget_models.hpp"
#include "test_mocks.hpp"
#include <memory>
#include <vector>
#include <string>

using namespace tb;
using namespace tb::normalizer;
using namespace tb::exchange::bitget;

// ============================================================
// Тестовая инфраструктура
// ============================================================

struct TestFixture {
    std::vector<NormalizedEvent> events;
    std::shared_ptr<tb::test::TestLogger> logger = std::make_shared<tb::test::TestLogger>();
    std::shared_ptr<tb::test::TestClock> clock = std::make_shared<tb::test::TestClock>();
    std::unique_ptr<BitgetNormalizer> normalizer;

    TestFixture() {
        // Время: 2023-11-14T22:13:20Z в наносекундах
        clock->current_time = 1'700'000'000'000'000'000LL;
        normalizer = std::make_unique<BitgetNormalizer>(
            [this](NormalizedEvent e) { events.push_back(std::move(e)); },
            clock, logger);
    }

    void send(const std::string& payload, int64_t received_ns = 1'700'000'000'000'000'000LL) {
        RawWsMessage msg;
        msg.type = WsMsgType::Unknown;
        msg.raw_payload = payload;
        msg.received_ns = received_ns;
        normalizer->process_raw_message(msg);
    }
};

// Шаблоны JSON для Bitget v2 USDT-M Futures
static std::string make_ticker_json(
    const std::string& instId = "BTCUSDT",
    const std::string& last = "50000.0",
    const std::string& bid = "49999.0",
    const std::string& ask = "50001.0",
    const std::string& volume = "100.0",
    const std::string& change = "0.02",
    const std::string& ts = "1700000000000",
    const std::string& instType = "USDT-FUTURES")
{
    return R"({"action":"snapshot","arg":{"instType":")" + instType +
           R"(","channel":"ticker","instId":")" + instId +
           R"("},"data":[{"instId":")" + instId +
           R"(","lastPr":")" + last +
           R"(","bidPr":")" + bid +
           R"(","askPr":")" + ask +
           R"(","baseVolume":")" + volume +
           R"(","change24h":")" + change +
           R"(","ts":")" + ts + R"("}]})";
}

static std::string make_trade_json(
    const std::string& instId = "BTCUSDT",
    const std::string& tradeId = "t001",
    const std::string& price = "50000.0",
    const std::string& size = "0.5",
    const std::string& side = "Sell",
    const std::string& ts = "1700000000000",
    const std::string& instType = "USDT-FUTURES")
{
    return R"({"arg":{"instType":")" + instType +
           R"(","channel":"trade","instId":")" + instId +
           R"("},"data":[{"tradeId":")" + tradeId +
           R"(","price":")" + price +
           R"(","size":")" + size +
           R"(","side":")" + side +
           R"(","ts":")" + ts + R"("}]})";
}

static std::string make_orderbook_json(
    const std::string& instId = "BTCUSDT",
    const std::string& action = "snapshot",
    const std::string& bids = R"([["49999.0","1.5"],["49998.0","2.0"]])",
    const std::string& asks = R"([["50001.0","1.0"],["50002.0","3.0"]])",
    const std::string& ts = "1700000000000",
    const std::string& seqId = "12345",
    const std::string& instType = "USDT-FUTURES")
{
    return R"({"action":")" + action +
           R"(","arg":{"instType":")" + instType +
           R"(","channel":"books15","instId":")" + instId +
           R"("},"data":[{"bids":)" + bids +
           R"(,"asks":)" + asks +
           R"(,"ts":")" + ts +
           R"(","seqId":")" + seqId + R"("}]})";
}

static std::string make_candle_json(
    const std::string& instId = "BTCUSDT",
    const std::string& interval = "1m",
    const std::string& ts = "1700000000000",
    const std::string& o = "50000.0",
    const std::string& h = "50100.0",
    const std::string& l = "49900.0",
    const std::string& c = "50050.0",
    const std::string& vol = "10.0",
    const std::string& baseVol = "500000.0",
    const std::string& isClosed = "",
    const std::string& instType = "USDT-FUTURES")
{
    std::string data = R"([")" + ts + R"(",")" + o + R"(",")" + h + R"(",")" + l +
                       R"(",")" + c + R"(",")" + vol + R"(",")" + baseVol + R"(")";
    if (!isClosed.empty()) {
        data += R"(,")" + isClosed + R"(")";
    }
    data += "]";

    return R"({"arg":{"instType":")" + instType +
           R"(","channel":"candle)" + interval +
           R"(","instId":")" + instId +
           R"("},"data":[)" + data + R"(]})";
}

// ============================================================
// TICKER TESTS
// ============================================================

TEST_CASE("Ticker: валидный USDT-M futures тикер парсится корректно", "[normalizer][ticker]") {
    TestFixture f;
    f.send(make_ticker_json());

    REQUIRE(f.events.size() == 1);
    const auto& ticker = std::get<NormalizedTicker>(f.events[0]);

    CHECK(ticker.last_price.get() == 50000.0);
    CHECK(ticker.bid.get() == 49999.0);
    CHECK(ticker.ask.get() == 50001.0);
    CHECK(ticker.volume_24h.get() == 100.0);
    CHECK(ticker.change_24h_pct == Catch::Approx(0.02));

    // Спред: ask - bid = 2.0, mid = 50000.0, bps = 2/50000 * 10000 = 0.4
    CHECK(ticker.spread == Catch::Approx(2.0));
    CHECK(ticker.spread_bps == Catch::Approx(0.4).epsilon(0.01));
}

TEST_CASE("Ticker: exchange_ts конвертируется из ms в ns", "[normalizer][ticker][timestamp]") {
    TestFixture f;
    f.send(make_ticker_json("ETHUSDT", "3000.0", "2999.0", "3001.0", "50.0", "0.01", "1700000000000"));

    REQUIRE(f.events.size() == 1);
    const auto& ticker = std::get<NormalizedTicker>(f.events[0]);

    // 1700000000000 ms * 1_000_000 = 1700000000000000000000 ns
    CHECK(ticker.envelope.exchange_ts.get() == 1'700'000'000'000'000'000LL);
    CHECK(ticker.envelope.source == "bitget");
    CHECK(ticker.envelope.symbol.get() == "ETHUSDT");
}

TEST_CASE("Ticker: отклоняется при нулевой цене", "[normalizer][ticker][validation]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "0.0", "49999.0", "50001.0"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Ticker: отклоняется при crossed book (ask < bid)", "[normalizer][ticker][validation]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "50000.0", "50002.0", "49998.0"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Ticker: отклоняется при отрицательных ценах", "[normalizer][ticker][validation]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "50000.0", "-1.0", "50001.0"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Ticker: нулевой спред при bid == ask допустим", "[normalizer][ticker]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "50000.0", "50000.0", "50000.0"));

    REQUIRE(f.events.size() == 1);
    const auto& ticker = std::get<NormalizedTicker>(f.events[0]);
    CHECK(ticker.spread == 0.0);
    CHECK(ticker.spread_bps == 0.0);
}

// ============================================================
// TRADE TESTS
// ============================================================

TEST_CASE("Trade: sell-side taker (Bitget v2 capitalized)", "[normalizer][trade]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t001", "50000.0", "0.5", "Sell"));

    REQUIRE(f.events.size() == 1);
    const auto& trade = std::get<NormalizedTrade>(f.events[0]);

    CHECK(trade.trade_id == "t001");
    CHECK(trade.price.get() == 50000.0);
    CHECK(trade.size.get() == 0.5);
    CHECK(trade.side == Side::Sell);
    CHECK(trade.is_aggressive == true); // sell-side taker = aggressive
}

TEST_CASE("Trade: buy-side taker (Bitget v2 capitalized)", "[normalizer][trade]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t002", "50000.0", "1.0", "Buy"));

    REQUIRE(f.events.size() == 1);
    const auto& trade = std::get<NormalizedTrade>(f.events[0]);

    CHECK(trade.side == Side::Buy);
    CHECK(trade.is_aggressive == false); // buy-side taker = not aggressive (sell pressure metric)
}

TEST_CASE("Trade: lowercase side (Bitget v1 legacy)", "[normalizer][trade]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t003", "50000.0", "0.1", "sell"));

    REQUIRE(f.events.size() == 1);
    const auto& trade = std::get<NormalizedTrade>(f.events[0]);

    CHECK(trade.side == Side::Sell);
    CHECK(trade.is_aggressive == true);
}

TEST_CASE("Trade: lowercase buy (Bitget v1 legacy)", "[normalizer][trade]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t004", "50000.0", "0.1", "buy"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedTrade>(f.events[0]).side == Side::Buy);
}

TEST_CASE("Trade: отклоняется при нулевой цене", "[normalizer][trade][validation]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t005", "0.0", "1.0", "Buy"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Trade: отклоняется при нулевом размере", "[normalizer][trade][validation]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t006", "50000.0", "0.0", "Buy"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Trade: exchange_ts конвертируется ms → ns", "[normalizer][trade][timestamp]") {
    TestFixture f;
    f.send(make_trade_json("BTCUSDT", "t007", "50000.0", "1.0", "Buy", "1700000000000"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedTrade>(f.events[0]).envelope.exchange_ts.get() == 1'700'000'000'000'000'000LL);
}

// ============================================================
// ORDER BOOK TESTS
// ============================================================

TEST_CASE("OrderBook: snapshot парсится корректно", "[normalizer][orderbook]") {
    TestFixture f;
    f.send(make_orderbook_json("BTCUSDT", "snapshot"));

    REQUIRE(f.events.size() == 1);
    const auto& book = std::get<NormalizedOrderBook>(f.events[0]);

    CHECK(book.update_type == BookUpdateType::Snapshot);
    REQUIRE(book.bids.size() == 2);
    CHECK(book.bids[0].price.get() == 49999.0);
    CHECK(book.bids[0].size.get() == 1.5);
    CHECK(book.bids[1].price.get() == 49998.0);
    CHECK(book.bids[1].size.get() == 2.0);

    REQUIRE(book.asks.size() == 2);
    CHECK(book.asks[0].price.get() == 50001.0);
    CHECK(book.asks[0].size.get() == 1.0);
    CHECK(book.asks[1].price.get() == 50002.0);
    CHECK(book.asks[1].size.get() == 3.0);
}

TEST_CASE("OrderBook: update → Delta", "[normalizer][orderbook]") {
    TestFixture f;
    f.send(make_orderbook_json("BTCUSDT", "update"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedOrderBook>(f.events[0]).update_type == BookUpdateType::Delta);
}

TEST_CASE("OrderBook: seqId передаётся в sequence", "[normalizer][orderbook]") {
    TestFixture f;
    f.send(make_orderbook_json("BTCUSDT", "snapshot",
           R"([["49999.0","1.0"]])", R"([["50001.0","1.0"]])",
           "1700000000000", "98765"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedOrderBook>(f.events[0]).sequence == 98765);
}

TEST_CASE("OrderBook: уровни с нулевой ценой отбрасываются", "[normalizer][orderbook][validation]") {
    TestFixture f;
    // Один невалидный уровень (price=0), один валидный
    f.send(make_orderbook_json("BTCUSDT", "snapshot",
           R"([["0.0","5.0"],["49999.0","1.0"]])", R"([["50001.0","1.0"]])"));

    REQUIRE(f.events.size() == 1);
    const auto& book = std::get<NormalizedOrderBook>(f.events[0]);
    CHECK(book.bids.size() == 1); // невалидный уровень отброшен
    CHECK(book.bids[0].price.get() == 49999.0);
}

TEST_CASE("OrderBook: size == 0 допустим (удаление уровня в delta)", "[normalizer][orderbook]") {
    TestFixture f;
    f.send(make_orderbook_json("BTCUSDT", "update",
           R"([["49999.0","0.0"]])", R"([["50001.0","0.0"]])"));

    REQUIRE(f.events.size() == 1);
    const auto& book = std::get<NormalizedOrderBook>(f.events[0]);
    CHECK(book.bids.size() == 1);
    CHECK(book.bids[0].size.get() == 0.0); // удаление уровня при delta
}

TEST_CASE("OrderBook: exchange_ts конвертируется ms → ns", "[normalizer][orderbook][timestamp]") {
    TestFixture f;
    f.send(make_orderbook_json());

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedOrderBook>(f.events[0]).envelope.exchange_ts.get() == 1'700'000'000'000'000'000LL);
}

// ============================================================
// CANDLE TESTS
// ============================================================

TEST_CASE("Candle: валидная закрытая свеча 1m", "[normalizer][candle]") {
    TestFixture f;
    // ts далеко в прошлом → свеча закрыта по временной проверке
    f.send(make_candle_json("BTCUSDT", "1m", "1699999900000",
           "50000.0", "50100.0", "49900.0", "50050.0", "10.0", "500000.0"));

    REQUIRE(f.events.size() == 1);
    const auto& candle = std::get<NormalizedCandle>(f.events[0]);

    CHECK(candle.interval == "1m");
    CHECK(candle.open.get() == 50000.0);
    CHECK(candle.high.get() == 50100.0);
    CHECK(candle.low.get() == 49900.0);
    CHECK(candle.close.get() == 50050.0);
    CHECK(candle.volume.get() == 10.0);
    CHECK(candle.base_volume.get() == 500000.0);
    CHECK(candle.is_closed == true);
}

TEST_CASE("Candle: явный is_closed флаг", "[normalizer][candle]") {
    TestFixture f;
    // Даже если время ещё не истекло, флаг "1" означает закрытие
    f.send(make_candle_json("BTCUSDT", "1m", "1700000000000",
           "50000.0", "50100.0", "49900.0", "50050.0", "10.0", "500000.0", "1"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedCandle>(f.events[0]).is_closed == true);
}

TEST_CASE("Candle: открытая свеча (недавний ts, нет флага)", "[normalizer][candle]") {
    TestFixture f;
    // clock->now() = 1700000000000000000 ns = 1700000000000 ms
    // ts = 1700000000000 ms → ts + 60000 = 1700000060000 > 1700000000000 → open
    f.send(make_candle_json("BTCUSDT", "1m", "1700000000000",
           "50000.0", "50100.0", "49900.0", "50050.0", "10.0", "500000.0"));

    REQUIRE(f.events.size() == 1);
    // ts + interval_ms > now_ms → свеча открыта
    CHECK(std::get<NormalizedCandle>(f.events[0]).is_closed == false);
}

TEST_CASE("Candle: отклоняется при high < low", "[normalizer][candle][validation]") {
    TestFixture f;
    f.send(make_candle_json("BTCUSDT", "1m", "1699999900000",
           "50000.0", "49800.0", "50200.0", "50050.0")); // high=49800 < low=50200

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Candle: отклоняется при нулевых OHLC", "[normalizer][candle][validation]") {
    TestFixture f;
    f.send(make_candle_json("BTCUSDT", "1m", "1699999900000",
           "0.0", "50100.0", "49900.0", "50050.0"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Candle: отклоняется при high < open (OHLC inconsistency)", "[normalizer][candle][validation]") {
    TestFixture f;
    // open=50100, high=50050 → high < open
    f.send(make_candle_json("BTCUSDT", "1m", "1699999900000",
           "50100.0", "50050.0", "49900.0", "50000.0"));

    CHECK(f.events.empty());
    CHECK(f.normalizer->rejected_count() == 1);
}

TEST_CASE("Candle: exchange_ts конвертируется ms → ns", "[normalizer][candle][timestamp]") {
    TestFixture f;
    f.send(make_candle_json("BTCUSDT", "1m", "1699999900000",
           "50000.0", "50100.0", "49900.0", "50050.0"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedCandle>(f.events[0]).envelope.exchange_ts.get() == 1'699'999'900'000'000'000LL);
}

TEST_CASE("Candle: интервал 5m парсится корректно", "[normalizer][candle]") {
    TestFixture f;
    // ts далеко в прошлом → закрыта
    f.send(make_candle_json("BTCUSDT", "5m", "1699999000000"));

    REQUIRE(f.events.size() == 1);
    const auto& candle = std::get<NormalizedCandle>(f.events[0]);
    CHECK(candle.interval == "5m");
    CHECK(candle.is_closed == true);
}

TEST_CASE("Candle: интервал 1h парсится корректно", "[normalizer][candle]") {
    TestFixture f;
    f.send(make_candle_json("BTCUSDT", "1H", "1699990000000"));

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedCandle>(f.events[0]).interval == "1H");
}

TEST_CASE("Candle: неизвестный интервал отклоняется", "[normalizer][candle]") {
    TestFixture f;
    f.send(make_candle_json("BTCUSDT", "7m", "1699999900000"));

    CHECK(f.events.empty()); // unknown interval → rejected at dispatch level
}

// ============================================================
// INST_TYPE FILTER (USDT-FUTURES only)
// ============================================================

TEST_CASE("InstType: SPOT сообщения отклоняются", "[normalizer][insttype]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "50000.0", "49999.0", "50001.0",
                            "100.0", "0.02", "1700000000000", "SPOT"));

    CHECK(f.events.empty());
}

TEST_CASE("InstType: COIN-FUTURES отклоняются", "[normalizer][insttype]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSD", "50000.0", "49999.0", "50001.0",
                            "100.0", "0.02", "1700000000000", "COIN-FUTURES"));

    CHECK(f.events.empty());
}

TEST_CASE("InstType: USDT-FUTURES принимается", "[normalizer][insttype]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT", "50000.0", "49999.0", "50001.0",
                            "100.0", "0.02", "1700000000000", "USDT-FUTURES"));

    CHECK(f.events.size() == 1);
}

TEST_CASE("InstType: отсутствие instType допускается", "[normalizer][insttype]") {
    TestFixture f;
    // Если instType не указан, сообщение пропускается (обратная совместимость)
    std::string payload = R"({"arg":{"channel":"ticker","instId":"BTCUSDT"},"data":[{"instId":"BTCUSDT","lastPr":"50000.0","bidPr":"49999.0","askPr":"50001.0","baseVolume":"100.0","change24h":"0.02","ts":"1700000000000"}]})";
    f.send(payload);

    CHECK(f.events.size() == 1);
}

// ============================================================
// SYMBOL FILTER
// ============================================================

TEST_CASE("Symbol filter: пропускает из разрешённого списка", "[normalizer][filter]") {
    TestFixture f;
    f.normalizer->set_symbols({Symbol{"BTCUSDT"}, Symbol{"ETHUSDT"}});
    f.send(make_ticker_json("BTCUSDT"));

    CHECK(f.events.size() == 1);
}

TEST_CASE("Symbol filter: отклоняет неизвестный символ", "[normalizer][filter]") {
    TestFixture f;
    f.normalizer->set_symbols({Symbol{"ETHUSDT"}});
    f.send(make_ticker_json("BTCUSDT"));

    CHECK(f.events.empty());
}

TEST_CASE("Symbol filter: пустой список = пропускать все", "[normalizer][filter]") {
    TestFixture f;
    // Не вызываем set_symbols → пустой список → всё пропускается
    f.send(make_ticker_json("XYZUSDT"));

    CHECK(f.events.size() == 1);
}

// ============================================================
// ENVELOPE TESTS
// ============================================================

TEST_CASE("Envelope: sequence_id монотонно возрастает", "[normalizer][envelope]") {
    TestFixture f;
    f.send(make_ticker_json("BTCUSDT"));
    f.send(make_trade_json("BTCUSDT"));
    f.send(make_ticker_json("ETHUSDT", "3000.0", "2999.0", "3001.0"));

    REQUIRE(f.events.size() == 3);
    CHECK(std::get<NormalizedTicker>(f.events[0]).envelope.sequence_id == 1);
    CHECK(std::get<NormalizedTrade>(f.events[1]).envelope.sequence_id == 2);
    CHECK(std::get<NormalizedTicker>(f.events[2]).envelope.sequence_id == 3);
}

TEST_CASE("Envelope: processed_ts берётся из инжектированного clock", "[normalizer][envelope]") {
    TestFixture f;
    f.clock->current_time = 42'000'000'000LL; // 42 секунды
    f.send(make_ticker_json());

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedTicker>(f.events[0]).envelope.processed_ts.get() == 42'000'000'000LL);
}

TEST_CASE("Envelope: received_ts передаётся из сообщения", "[normalizer][envelope]") {
    TestFixture f;
    const int64_t recv = 1'600'000'000'000'000'000LL;
    f.send(make_ticker_json(), recv);

    REQUIRE(f.events.size() == 1);
    CHECK(std::get<NormalizedTicker>(f.events[0]).envelope.received_ts.get() == recv);
}

// ============================================================
// ROBUSTNESS / EDGE CASES
// ============================================================

TEST_CASE("Robustness: не-объект JSON не вызывает крэш", "[normalizer][robustness]") {
    TestFixture f;
    f.send("[1, 2, 3]");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: JSON без arg не вызывает крэш", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"data":[{"foo":"bar"}]})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: JSON без data не вызывает крэш", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"arg":{"channel":"ticker","instId":"BTCUSDT"}})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: пустой data массив не вызывает крэш", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"arg":{"channel":"ticker","instId":"BTCUSDT"},"data":[]})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: неизвестный канал не вызывает крэш", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"arg":{"channel":"unknown_channel","instId":"BTCUSDT"},"data":[{}]})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: data содержит не-объект для ticker", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"arg":{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"},"data":["string_not_object"]})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: candle с < 7 элементами пропускается", "[normalizer][robustness]") {
    TestFixture f;
    f.send(R"({"arg":{"instType":"USDT-FUTURES","channel":"candle1m","instId":"BTCUSDT"},"data":[["1700000000000","50000.0","50100.0"]]})");
    CHECK(f.events.empty());
}

TEST_CASE("Robustness: множественные сделки в одном data", "[normalizer][robustness]") {
    TestFixture f;
    std::string payload = R"({"arg":{"instType":"USDT-FUTURES","channel":"trade","instId":"BTCUSDT"},"data":[)" 
        R"({"tradeId":"t1","price":"50000.0","size":"0.5","side":"Sell","ts":"1700000000000"},)"
        R"({"tradeId":"t2","price":"50001.0","size":"1.0","side":"Buy","ts":"1700000000001"}]})";
    f.send(payload);

    REQUIRE(f.events.size() == 2);
    CHECK(std::get<NormalizedTrade>(f.events[0]).trade_id == "t1");
    CHECK(std::get<NormalizedTrade>(f.events[1]).trade_id == "t2");
}

// ============================================================
// CANDLE INTERVAL COVERAGE
// ============================================================

TEST_CASE("Candle intervals: все стандартные USDT-M интервалы поддерживаются", "[normalizer][candle][intervals]") {
    const std::vector<std::string> intervals = {
        "1m", "3m", "5m", "15m", "30m",
        "1h", "1H", "2h", "4h", "6h", "12h",
        "1d", "1D", "1w", "1W", "1M"
    };

    for (const auto& interval : intervals) {
        TestFixture f;
        // ts далеко в прошлом → свеча закрыта
        f.send(make_candle_json("BTCUSDT", interval, "1000000000000"));
        INFO("Interval: " << interval);
        CHECK(f.events.size() == 1);
    }
}
