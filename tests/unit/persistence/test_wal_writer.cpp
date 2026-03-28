/**
 * @file test_wal_writer.cpp
 * @brief Тесты Write-Ahead Logger (WalWriter)
 */
#include <catch2/catch_test_macros.hpp>
#include "persistence/wal_writer.hpp"
#include "persistence/event_journal.hpp"
#include "persistence/memory_storage_adapter.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

using namespace tb;
using namespace tb::persistence;

// ========== Тестовые заглушки ==========

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

class TestClock : public clock::IClock {
public:
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
};

class TestMetrics : public metrics::IMetricsRegistry {
    struct NullCounter : metrics::ICounter {
        std::string name_{"null"};
        void increment(double) override {}
        void increment(double, const metrics::MetricTags&) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullGauge : metrics::IGauge {
        std::string name_{"null"};
        void set(double) override {}
        void set(double, const metrics::MetricTags&) override {}
        void increment(double) override {}
        void decrement(double) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullHistogram : metrics::IHistogram {
        std::string name_{"null"};
        void observe(double) override {}
        void observe(double, const metrics::MetricTags&) override {}
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override {
        return std::make_shared<NullCounter>();
    }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override {
        return std::make_shared<NullGauge>();
    }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override {
        return std::make_shared<NullHistogram>();
    }
    [[nodiscard]] std::string export_prometheus() const override { return ""; }
};

// ========== Вспомогательные функции ==========

inline auto make_wal_writer() {
    auto adapter = std::make_shared<MemoryStorageAdapter>();
    auto journal = std::make_shared<EventJournal>(adapter);
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto wal = std::make_shared<WalWriter>(journal, logger, clk, met);
    return std::make_tuple(wal, journal, adapter);
}

// ========== Тесты ==========

TEST_CASE("WalWriter: write_intent и commit", "[persistence][wal]") {
    auto [wal, journal, adapter] = make_wal_writer();

    auto seq_result = wal->write_intent(
        WalEntryType::OrderIntent,
        R"({"symbol":"BTCUSDT","side":"Buy"})",
        OrderId("ORD-1"),
        Symbol("BTCUSDT"),
        StrategyId("strat-1"));

    REQUIRE(seq_result.has_value());
    uint64_t seq = *seq_result;
    REQUIRE(seq > 0);

    // Commit должен быть успешным
    auto commit_result = wal->commit(seq);
    REQUIRE(commit_result.has_value());

    // После commit запись не должна быть в uncommitted
    auto uncommitted = wal->find_uncommitted();
    REQUIRE(uncommitted.has_value());
    for (const auto& entry : *uncommitted) {
        REQUIRE(entry.wal_sequence != seq);
    }
}

TEST_CASE("WalWriter: write_intent и rollback", "[persistence][wal]") {
    auto [wal, journal, adapter] = make_wal_writer();

    auto seq_result = wal->write_intent(
        WalEntryType::OrderIntent,
        R"({"symbol":"ETHUSDT","side":"Sell"})",
        OrderId("ORD-2"),
        Symbol("ETHUSDT"));

    REQUIRE(seq_result.has_value());
    uint64_t seq = *seq_result;

    // Rollback
    auto rollback_result = wal->rollback(seq, "Exchange rejected");
    REQUIRE(rollback_result.has_value());

    // После rollback запись не должна быть в uncommitted
    auto uncommitted = wal->find_uncommitted();
    REQUIRE(uncommitted.has_value());
    for (const auto& entry : *uncommitted) {
        REQUIRE(entry.wal_sequence != seq);
    }
}

TEST_CASE("WalWriter: find_uncommitted", "[persistence][wal]") {
    auto [wal, journal, adapter] = make_wal_writer();

    // Пишем два intent-а, один коммитим, другой нет
    auto seq1_result = wal->write_intent(
        WalEntryType::OrderIntent,
        R"({"order":1})",
        OrderId("ORD-A"));
    REQUIRE(seq1_result.has_value());
    uint64_t seq1 = *seq1_result;

    auto seq2_result = wal->write_intent(
        WalEntryType::PositionOpened,
        R"({"order":2})",
        OrderId("ORD-B"));
    REQUIRE(seq2_result.has_value());
    uint64_t seq2 = *seq2_result;

    // Коммитим только первый
    wal->commit(seq1);

    // find_uncommitted должен вернуть только второй
    auto uncommitted = wal->find_uncommitted();
    REQUIRE(uncommitted.has_value());

    bool found_seq2 = false;
    for (const auto& entry : *uncommitted) {
        REQUIRE(entry.wal_sequence != seq1);  // seq1 закоммичен
        if (entry.wal_sequence == seq2) {
            found_seq2 = true;
        }
    }
    REQUIRE(found_seq2);
}

TEST_CASE("WalWriter: checkpoint запись", "[persistence][wal]") {
    auto [wal, journal, adapter] = make_wal_writer();

    auto result = wal->write_checkpoint(R"({"state":"snapshot","positions":[]})");
    REQUIRE(result.has_value());

    // Flush должен работать
    auto flush_result = wal->flush();
    REQUIRE(flush_result.has_value());
}
