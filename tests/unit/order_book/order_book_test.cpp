#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "order_book/order_book.hpp"
#include "normalizer/normalized_events.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"

// Создаёт минимальный логгер для тестов (уровень Error, без лишнего вывода)
inline std::shared_ptr<tb::logging::ILogger> make_test_logger() {
    return tb::logging::create_console_logger(tb::logging::LogLevel::Error);
}

// Создаёт реестр метрик для тестов
inline std::shared_ptr<tb::metrics::IMetricsRegistry> make_test_metrics() {
    return tb::metrics::create_metrics_registry();
}

// Создаёт нормализованный снимок стакана для теста
inline tb::normalizer::NormalizedOrderBook make_snapshot(
    std::vector<std::pair<double, double>> bids,
    std::vector<std::pair<double, double>> asks,
    uint64_t sequence = 1)
{
    tb::normalizer::NormalizedOrderBook book;
    book.update_type = tb::normalizer::BookUpdateType::Snapshot;
    book.sequence = sequence;
    book.envelope.symbol = tb::Symbol("BTCUSDT");
    book.envelope.sequence_id = sequence;
    for (auto& [p, s] : bids)
        book.bids.push_back({tb::Price(p), tb::Quantity(s)});
    for (auto& [p, s] : asks)
        book.asks.push_back({tb::Price(p), tb::Quantity(s)});
    return book;
}

// Создаёт нормализованное дельта-обновление стакана для теста
inline tb::normalizer::NormalizedOrderBook make_delta(
    std::vector<std::pair<double, double>> bids,
    std::vector<std::pair<double, double>> asks,
    uint64_t sequence)
{
    tb::normalizer::NormalizedOrderBook book;
    book.update_type = tb::normalizer::BookUpdateType::Delta;
    book.sequence = sequence;
    book.envelope.symbol = tb::Symbol("BTCUSDT");
    book.envelope.sequence_id = sequence;
    for (auto& [p, s] : bids)
        book.bids.push_back({tb::Price(p), tb::Quantity(s)});
    for (auto& [p, s] : asks)
        book.asks.push_back({tb::Price(p), tb::Quantity(s)});
    return book;
}

// ============================================================
// Тесты LocalOrderBook
// ============================================================

TEST_CASE("LocalOrderBook: apply_snapshot → качество Valid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot(
        {{100.0, 1.0}, {99.0, 2.0}},
        {{101.0, 0.5}}));

    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
    REQUIRE(book.top_of_book().has_value());
}

TEST_CASE("LocalOrderBook: apply_delta с правильным sequence → Valid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 1.0}}, 1));

    bool ok = book.apply_delta(make_delta({{100.0, 1.5}}, {}, 2));

    REQUIRE(ok == true);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

TEST_CASE("LocalOrderBook: apply_delta с неправильным sequence → Desynced") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 1.0}}, 1));

    // Пропуск sequence — стакан должен уйти в Desynced
    bool ok = book.apply_delta(make_delta({{100.0, 2.0}}, {}, 5));

    REQUIRE(ok == false);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Desynced);
}

TEST_CASE("LocalOrderBook: request_resync → Uninitialized") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 1.0}}));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);

    book.request_resync();

    REQUIRE(book.quality() == tb::order_book::BookQuality::Uninitialized);
}

TEST_CASE("LocalOrderBook: top_of_book вычисляет правильный bid/ask") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot(
        {{100.0, 1.0}, {99.0, 2.0}},
        {{101.0, 0.5}}));

    auto top = book.top_of_book();
    REQUIRE(top.has_value());
    REQUIRE(top->best_bid.get() == Catch::Approx(100.0));
    REQUIRE(top->best_ask.get() == Catch::Approx(101.0));
    REQUIRE(top->spread == Catch::Approx(1.0));
    REQUIRE(top->mid_price == Catch::Approx(100.5));
}

TEST_CASE("LocalOrderBook: depth_summary вычисляет дисбаланс") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Равный объём на обеих сторонах → дисбаланс ≈ 0
    book.apply_snapshot(make_snapshot(
        {{100.0, 1.0}, {99.0, 1.0}},
        {{101.0, 1.0}, {102.0, 1.0}}));

    auto depth = book.depth_summary(10);
    REQUIRE(depth.has_value());
    REQUIRE(depth->imbalance_5 == Catch::Approx(0.0).margin(0.01));

    // Только биды — дисбаланс должен быть > 0
    book.apply_snapshot(make_snapshot(
        {{100.0, 5.0}, {99.0, 5.0}},
        {{101.0, 0.001}}));

    auto depth2 = book.depth_summary(10);
    REQUIRE(depth2.has_value());
    REQUIRE(depth2->imbalance_5 > 0.0);
}

TEST_CASE("LocalOrderBook: уровень с size=0 удаляет уровень") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {}, 1));
    REQUIRE(book.bids().size() == 1);

    // Дельта с size=0 удаляет уровень
    bool ok = book.apply_delta(make_delta({{100.0, 0.0}}, {}, 2));
    REQUIRE(ok == true);
    REQUIRE(book.bids().empty());
    // Стакан без бидов и асков — top_of_book должен быть пустым
    REQUIRE_FALSE(book.top_of_book().has_value());
}
