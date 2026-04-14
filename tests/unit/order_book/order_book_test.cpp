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
    uint64_t sequence = 1,
    const std::string& symbol = "BTCUSDT")
{
    tb::normalizer::NormalizedOrderBook book;
    book.update_type = tb::normalizer::BookUpdateType::Snapshot;
    book.sequence = sequence;
    book.envelope.symbol = tb::Symbol(symbol);
    book.envelope.sequence_id = sequence;
    book.envelope.processed_ts = tb::Timestamp{1000000000LL};
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
    uint64_t sequence,
    const std::string& symbol = "BTCUSDT")
{
    tb::normalizer::NormalizedOrderBook book;
    book.update_type = tb::normalizer::BookUpdateType::Delta;
    book.sequence = sequence;
    book.envelope.symbol = tb::Symbol(symbol);
    book.envelope.sequence_id = sequence;
    book.envelope.processed_ts = tb::Timestamp{2000000000LL};
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

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));
    REQUIRE(book.bids().size() == 1);

    // Дельта с size=0 удаляет уровень
    bool ok = book.apply_delta(make_delta({{100.0, 0.0}}, {}, 2));
    REQUIRE(ok == true);
    REQUIRE(book.bids().empty());
    // Стакан без бидов — top_of_book должен быть пустым
    REQUIRE_FALSE(book.top_of_book().has_value());
}

// ============================================================
// Новые тесты: валидация символа
// ============================================================

TEST_CASE("LocalOrderBook: snapshot с неверным символом отклоняется") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Snapshot с другим символом не должен применяться
    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1, "ETHUSDT"));

    REQUIRE(book.quality() == tb::order_book::BookQuality::Uninitialized);
    REQUIRE(book.bids().empty());
}

TEST_CASE("LocalOrderBook: delta с неверным символом отклоняется") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);

    bool ok = book.apply_delta(make_delta({{100.0, 2.0}}, {}, 2, "ETHUSDT"));
    REQUIRE(ok == false);
    // Книга остаётся Valid — это не desync, а ошибка маршрутизации
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

// ============================================================
// Новые тесты: кроссированный стакан
// ============================================================

TEST_CASE("LocalOrderBook: кроссированный snapshot → Desynced") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // best_bid (102) >= best_ask (101) — кроссированный стакан
    book.apply_snapshot(make_snapshot(
        {{102.0, 1.0}},
        {{101.0, 0.5}}, 1));

    REQUIRE(book.quality() == tb::order_book::BookQuality::Desynced);
}

TEST_CASE("LocalOrderBook: кроссированный стакан после delta → Desynced") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);

    // Delta делает стакан кроссированным: bid 102 >= ask 101
    bool ok = book.apply_delta(make_delta({{102.0, 1.0}}, {}, 2));
    REQUIRE(ok == false);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Desynced);
}

// ============================================================
// Новые тесты: невалидные данные (price <= 0, size < 0)
// ============================================================

TEST_CASE("LocalOrderBook: уровни с невалидной ценой или объёмом фильтруются") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    auto snap = make_snapshot({}, {}, 1);
    // Валидные уровни
    snap.bids.push_back({tb::Price(100.0), tb::Quantity(1.0)});
    snap.asks.push_back({tb::Price(101.0), tb::Quantity(1.0)});
    // Невалидные: price <= 0
    snap.bids.push_back({tb::Price(0.0), tb::Quantity(5.0)});
    snap.bids.push_back({tb::Price(-1.0), tb::Quantity(5.0)});
    // Невалидные: size < 0
    snap.asks.push_back({tb::Price(102.0), tb::Quantity(-1.0)});

    book.apply_snapshot(snap);

    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
    REQUIRE(book.bids().size() == 1);  // Только price=100
    REQUIRE(book.asks().size() == 1);  // Только price=101
}

// ============================================================
// Новые тесты: microprice (Stoikov 2018)
// ============================================================

TEST_CASE("LocalOrderBook: microprice при симметричном стакане ≈ mid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Симметричный стакан: равные объёмы → microprice ≈ mid
    book.apply_snapshot(make_snapshot(
        {{100.0, 1.0}},
        {{102.0, 1.0}}, 1));

    auto depth = book.depth_summary(10);
    REQUIRE(depth.has_value());
    // VWAP_bid=100, VWAP_ask=102, V_bid=V_ask=1
    // microprice = 102 * (1/2) + 100 * (1/2) = 101 = mid
    REQUIRE(depth->weighted_mid == Catch::Approx(101.0));
}

TEST_CASE("LocalOrderBook: microprice при асимметричном стакане сдвигается к ask") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Сильный bid (10.0) >> ask (1.0) → microprice > mid (ближе к ask)
    // Интуиция: давление покупки → цена пойдёт вверх
    book.apply_snapshot(make_snapshot(
        {{100.0, 10.0}},
        {{102.0, 1.0}}, 1));

    auto depth = book.depth_summary(10);
    REQUIRE(depth.has_value());
    // VWAP_bid=100, VWAP_ask=102, V_bid=10, V_ask=1
    // microprice = 102*(10/11) + 100*(1/11) = (1020+100)/11 ≈ 101.818
    const double mid = 101.0;
    REQUIRE(depth->weighted_mid > mid);
    REQUIRE(depth->weighted_mid == Catch::Approx(101.818).margin(0.01));
}

TEST_CASE("LocalOrderBook: microprice при доминирующем ask сдвигается к bid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Сильный ask (10.0) >> bid (1.0) → microprice < mid (ближе к bid)
    // Интуиция: давление продажи → цена пойдёт вниз
    book.apply_snapshot(make_snapshot(
        {{100.0, 1.0}},
        {{102.0, 10.0}}, 1));

    auto depth = book.depth_summary(10);
    REQUIRE(depth.has_value());
    // VWAP_bid=100, VWAP_ask=102, V_bid=1, V_ask=10
    // microprice = 102*(1/11) + 100*(10/11) = (102+1000)/11 ≈ 100.182
    const double mid = 101.0;
    REQUIRE(depth->weighted_mid < mid);
    REQUIRE(depth->weighted_mid == Catch::Approx(100.182).margin(0.01));
}

// ============================================================
// Новые тесты: staleness
// ============================================================

TEST_CASE("LocalOrderBook: check_staleness на свежей книге → false") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));

    // Книга обновлена в processed_ts=1000000000, проверяем через 1 секунду
    const int64_t stale_threshold = 3'000'000'000LL; // 3 секунды
    bool stale = book.check_staleness(tb::Timestamp{2'000'000'000LL}, stale_threshold);

    REQUIRE(stale == false);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

TEST_CASE("LocalOrderBook: check_staleness на устаревшей книге → Stale") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));

    // Книга обновлена в processed_ts=1000000000, проверяем через 5 секунд
    const int64_t stale_threshold = 3'000'000'000LL; // 3 секунды
    bool stale = book.check_staleness(tb::Timestamp{5'000'000'000LL}, stale_threshold);

    REQUIRE(stale == true);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Stale);
}

TEST_CASE("LocalOrderBook: новый snapshot после Stale восстанавливает Valid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));
    book.check_staleness(tb::Timestamp{5'000'000'000LL}, 3'000'000'000LL);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Stale);

    // Новый snapshot должен восстановить Valid
    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 10));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

TEST_CASE("LocalOrderBook: delta после Stale восстанавливает Valid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 1));
    book.check_staleness(tb::Timestamp{5'000'000'000LL}, 3'000'000'000LL);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Stale);

    // Корректная дельта должна восстановить Valid
    bool ok = book.apply_delta(make_delta({{100.0, 2.0}}, {}, 2));
    REQUIRE(ok == true);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

// ============================================================
// Новые тесты: sequence edge cases
// ============================================================

TEST_CASE("LocalOrderBook: sequence=0 snapshot + delta с gap → Desynced") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    // Snapshot с sequence=0
    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 0));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);

    // Delta с sequence=5 — gap от 0, должно быть Desynced
    bool ok = book.apply_delta(make_delta({{100.0, 2.0}}, {}, 5));
    REQUIRE(ok == false);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Desynced);
}

TEST_CASE("LocalOrderBook: sequence=0 snapshot + delta sequence=1 → Valid") {
    auto book = tb::order_book::LocalOrderBook(
        tb::Symbol("BTCUSDT"), make_test_logger(), make_test_metrics());

    book.apply_snapshot(make_snapshot({{100.0, 1.0}}, {{101.0, 0.5}}, 0));
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);

    // Delta с sequence=1 — корректная последовательность
    bool ok = book.apply_delta(make_delta({{100.0, 2.0}}, {}, 1));
    REQUIRE(ok == true);
    REQUIRE(book.quality() == tb::order_book::BookQuality::Valid);
}

// ============================================================
// Новые тесты: to_string
// ============================================================

TEST_CASE("BookQuality: to_string для всех значений") {
    using tb::order_book::BookQuality;
    using tb::order_book::to_string;

    REQUIRE(std::string(to_string(BookQuality::Valid)) == "valid");
    REQUIRE(std::string(to_string(BookQuality::Stale)) == "stale");
    REQUIRE(std::string(to_string(BookQuality::Desynced)) == "desynced");
    REQUIRE(std::string(to_string(BookQuality::Uninitialized)) == "uninitialized");
}
