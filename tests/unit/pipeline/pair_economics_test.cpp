/**
 * @file pair_economics_test.cpp
 * @brief Unit tests for PairEconomicsTracker (Phase 7)
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pipeline/pair_economics.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::pipeline;

namespace {

class TestClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    int64_t now_ns_{1'000'000'000'000LL};
};

PairEconomicsRecord make_winning_record() {
    PairEconomicsRecord rec;
    rec.symbol = Symbol("BTCUSDT");
    rec.strategy_id = StrategyId("momentum_v1");
    rec.correlation_id = CorrelationId("corr_001");
    rec.primary_gross_pnl = -5.0;
    rec.hedge_gross_pnl = 12.0;
    rec.gross_pair_pnl = 7.0;
    rec.net_pair_pnl = 5.5;
    rec.total_fees = 1.2;
    rec.total_funding = 0.3;
    rec.total_slippage = 0.0;
    rec.time_in_pair_ns = 120'000'000'000LL;  // 2 min
    rec.primary_hold_ns = 300'000'000'000LL;
    rec.hedge_hold_ns = 120'000'000'000LL;
    rec.exit_efficiency = 0.75;
    rec.primary_exit_reason = ReasonCode::ExitContinuationValue;
    rec.hedge_exit_reason = ReasonCode::HedgeCloseProfitLock;
    rec.primary_size = 0.01;
    rec.hedge_size = 0.008;
    rec.hedge_ratio_actual = 0.8;
    rec.opened_at = Timestamp(800'000'000'000LL);
    rec.closed_at = Timestamp(1'000'000'000'000LL);
    return rec;
}

PairEconomicsRecord make_losing_record() {
    auto rec = make_winning_record();
    rec.primary_gross_pnl = -15.0;
    rec.hedge_gross_pnl = 8.0;
    rec.gross_pair_pnl = -7.0;
    rec.net_pair_pnl = -8.5;
    rec.exit_efficiency = 0.2;
    rec.hedge_exit_reason = ReasonCode::HedgeCloseBothEmergency;
    return rec;
}

} // namespace

TEST_CASE("PairEconomicsTracker: empty tracker has zero stats", "[pair_economics]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PairEconomicsTracker tracker(logger, clk);

    auto stats = tracker.daily_stats();
    CHECK(stats.pair_cycles == 0);
    CHECK(stats.total_net_pnl == 0.0);
}

TEST_CASE("PairEconomicsTracker: record winning pair", "[pair_economics][win]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PairEconomicsTracker tracker(logger, clk);

    tracker.record(make_winning_record());

    auto stats = tracker.daily_stats();
    REQUIRE(stats.pair_cycles == 1);
    CHECK(stats.total_net_pnl > 0.0);
    CHECK(stats.total_fees > 0.0);
    CHECK(stats.win_rate == 1.0);
}

TEST_CASE("PairEconomicsTracker: mixed win/loss records", "[pair_economics][mixed]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PairEconomicsTracker tracker(logger, clk);

    tracker.record(make_winning_record());
    tracker.record(make_losing_record());

    auto stats = tracker.daily_stats();
    REQUIRE(stats.pair_cycles == 2);
    CHECK(stats.win_rate == 0.5);
    CHECK(stats.total_gross_pnl == 0.0);  // 7 + (-7) = 0
}

TEST_CASE("PairEconomicsTracker: recent returns latest records", "[pair_economics][recent]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PairEconomicsTracker tracker(logger, clk);

    for (int i = 0; i < 5; i++) tracker.record(make_winning_record());

    auto recent = tracker.recent(3);
    REQUIRE(recent.size() == 3);
}

TEST_CASE("PairEconomicsTracker: daily reset clears stats", "[pair_economics][reset]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PairEconomicsTracker tracker(logger, clk);

    tracker.record(make_winning_record());
    REQUIRE(tracker.daily_stats().pair_cycles == 1);

    tracker.reset_daily();
    CHECK(tracker.daily_stats().pair_cycles == 0);
    CHECK(tracker.daily_stats().total_net_pnl == 0.0);
}

TEST_CASE("PairEconomicsTracker: with metrics registry", "[pair_economics][metrics]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<metrics::InMemoryMetricsRegistry>();
    PairEconomicsTracker tracker(logger, clk, metrics);

    tracker.record(make_winning_record());
    tracker.record(make_losing_record());

    // Verify Prometheus export has our metrics
    auto prom = metrics->export_prometheus();
    CHECK(prom.find("tb_pair_cycles_total") != std::string::npos);
    CHECK(prom.find("tb_pair_net_pnl_usd") != std::string::npos);
}
