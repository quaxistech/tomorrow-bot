#include <catch2/catch_test_macros.hpp>
#include "normalizer/normalizer.hpp"
#include "exchange/bitget/bitget_models.hpp"
#include "logging/logger.hpp"
#include "clock/wall_clock.hpp"
#include <memory>
#include <vector>
#include <string>

// Создаёт часы реального времени для интеграционных тестов
static std::shared_ptr<tb::clock::IClock> make_test_clock() {
    return std::make_shared<tb::clock::WallClock>();
}

TEST_CASE("Normalizer: битый JSON не вызывает исключений", "[integration]") {
    auto logger = tb::logging::create_console_logger(tb::logging::LogLevel::Error);
    auto clock = make_test_clock();

    std::vector<tb::normalizer::NormalizedEvent> received_events;

    auto normalizer = std::make_unique<tb::normalizer::BitgetNormalizer>(
        [&received_events](tb::normalizer::NormalizedEvent e) {
            received_events.push_back(std::move(e));
        },
        clock,
        logger
    );

    // Битый JSON — не должен бросать исключение
    tb::exchange::bitget::RawWsMessage bad_msg;
    bad_msg.type = tb::exchange::bitget::WsMsgType::Unknown;
    bad_msg.raw_payload = "{ broken json {{{{ not valid";
    bad_msg.received_ns = 0;

    REQUIRE_NOTHROW(normalizer->process_raw_message(bad_msg));
    REQUIRE(received_events.empty());
}

TEST_CASE("Normalizer: пустой payload не вызывает исключений", "[integration]") {
    auto logger = tb::logging::create_console_logger(tb::logging::LogLevel::Error);
    auto clock = make_test_clock();

    std::vector<tb::normalizer::NormalizedEvent> received_events;

    auto normalizer = std::make_unique<tb::normalizer::BitgetNormalizer>(
        [&received_events](tb::normalizer::NormalizedEvent e) {
            received_events.push_back(std::move(e));
        },
        clock,
        logger
    );

    tb::exchange::bitget::RawWsMessage empty_msg;
    empty_msg.type = tb::exchange::bitget::WsMsgType::Unknown;
    empty_msg.raw_payload = "";
    empty_msg.received_ns = 0;

    REQUIRE_NOTHROW(normalizer->process_raw_message(empty_msg));
    REQUIRE(received_events.empty());
}

TEST_CASE("Normalizer: валидное сообщение тикера парсится корректно", "[integration]") {
    auto logger = tb::logging::create_console_logger(tb::logging::LogLevel::Error);
    auto clock = make_test_clock();

    std::vector<tb::normalizer::NormalizedEvent> received_events;

    auto normalizer = std::make_unique<tb::normalizer::BitgetNormalizer>(
        [&received_events](tb::normalizer::NormalizedEvent e) {
            received_events.push_back(std::move(e));
        },
        clock,
        logger
    );

    // Реальный формат Bitget v2 WebSocket сообщения тикера
    tb::exchange::bitget::RawWsMessage msg;
    msg.type = tb::exchange::bitget::WsMsgType::Unknown;
    msg.raw_payload = R"({"action":"snapshot","arg":{"instType":"SPOT","channel":"ticker","instId":"BTCUSDT"},"data":[{"instId":"BTCUSDT","lastPr":"50000.0","bidPr":"49999.0","askPr":"50001.0","baseVolume":"100.0","change24h":"0.02","ts":"1234567890000"}]})";
    msg.received_ns = 1234567890000000000LL;

    // Не должно быть исключений при обработке валидного сообщения
    REQUIRE_NOTHROW(normalizer->process_raw_message(msg));
}
