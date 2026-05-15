#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "normalizer/normalizer.hpp"
#include "exchange/bitget/bitget_models.hpp"
#include "test_mocks.hpp"
#include <memory>
#include <vector>
#include <string>

// Фабрика тестового окружения normalizer-а
static auto make_test_normalizer(std::vector<tb::normalizer::NormalizedEvent>& events) {
    auto logger = std::make_shared<tb::test::TestLogger>();
    auto clock = std::make_shared<tb::test::TestClock>();
    clock->current_time = 1'700'000'000'000'000'000LL; // ~2023-11-14 in ns
    auto normalizer = std::make_unique<tb::normalizer::BitgetNormalizer>(
        [&events](tb::normalizer::NormalizedEvent e) { events.push_back(std::move(e)); },
        clock, logger);
    return normalizer;
}

TEST_CASE("Normalizer: битый JSON не вызывает исключений", "[integration]") {
    std::vector<tb::normalizer::NormalizedEvent> events;
    auto normalizer = make_test_normalizer(events);

    tb::exchange::bitget::RawWsMessage bad_msg;
    bad_msg.type = tb::exchange::bitget::WsMsgType::Unknown;
    bad_msg.raw_payload = "{ broken json {{{{ not valid";
    bad_msg.received_ns = 0;

    REQUIRE_NOTHROW(normalizer->process_raw_message(bad_msg));
    REQUIRE(events.empty());
}

TEST_CASE("Normalizer: пустой payload не вызывает исключений", "[integration]") {
    std::vector<tb::normalizer::NormalizedEvent> events;
    auto normalizer = make_test_normalizer(events);

    tb::exchange::bitget::RawWsMessage empty_msg;
    empty_msg.type = tb::exchange::bitget::WsMsgType::Unknown;
    empty_msg.raw_payload = "";
    empty_msg.received_ns = 0;

    REQUIRE_NOTHROW(normalizer->process_raw_message(empty_msg));
    REQUIRE(events.empty());
}

TEST_CASE("Normalizer: валидное USDT-M futures тикер-сообщение парсится корректно", "[integration]") {
    std::vector<tb::normalizer::NormalizedEvent> events;
    auto normalizer = make_test_normalizer(events);

    // Bitget v2 USDT-M Futures формат тикера
    tb::exchange::bitget::RawWsMessage msg;
    msg.type = tb::exchange::bitget::WsMsgType::Ticker;
    msg.raw_payload = R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"},"data":[{"instId":"BTCUSDT","lastPr":"50000.0","bidPr":"49999.0","askPr":"50001.0","baseVolume":"100.0","change24h":"0.02","ts":"1700000000000"}]})";
    msg.received_ns = 1'700'000'000'000'000'000LL;

    REQUIRE_NOTHROW(normalizer->process_raw_message(msg));
    REQUIRE(events.size() == 1);

    const auto& ticker = std::get<tb::normalizer::NormalizedTicker>(events[0]);
    CHECK(ticker.last_price.get() == 50000.0);
    CHECK(ticker.bid.get() == 49999.0);
    CHECK(ticker.ask.get() == 50001.0);
    CHECK(ticker.volume_24h.get() == 100.0);
    CHECK(ticker.spread == Catch::Approx(2.0));
    CHECK(ticker.spread_bps == Catch::Approx(0.4).epsilon(0.01));
    // exchange_ts: 1700000000000 ms → ns
    CHECK(ticker.envelope.exchange_ts.get() == 1'700'000'000'000'000'000LL);
    CHECK(ticker.envelope.symbol.get() == "BTCUSDT");
    CHECK(ticker.envelope.source == "bitget");
}
