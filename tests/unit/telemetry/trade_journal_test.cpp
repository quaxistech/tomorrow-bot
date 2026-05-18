/**
 * @file trade_journal_test.cpp
 * @brief Тесты для TradeJournal (edge-31 Phase 6).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "telemetry/trade_journal.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <memory>

using namespace tb;
using namespace tb::telemetry;
using namespace Catch::Matchers;

namespace {

class FakeClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    void set(int64_t ns) { now_ns_ = ns; }
    void advance(int64_t ns) { now_ns_ += ns; }
private:
    int64_t now_ns_{0};
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TradeJournal: entry+exit produces a closed row with signal_age",
          "[trade_journal][edge-31]") {
    auto log = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<FakeClock>();
    clk->set(1'000'000'000'000LL);  // 1000s

    TradeJournal j(log, clk);

    // Signal был сформирован за 80ms до fill.
    const int64_t snapshot_ts = clk->now().get() - 80'000'000;
    j.on_entry_filled(Symbol("BTCUSDT"), PositionSide::Long,
                       50000.0, 0.01, snapshot_ts, "scalp", "MomentumContinuation", 0.85);

    auto in_flight = j.get_in_flight(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(in_flight.has_value());
    REQUIRE(in_flight->row.signal_age_ms == 80);

    // Прошло 10 минут, mid цена ходила выше.
    clk->advance(600'000'000'000LL);  // +10 min
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Long, 50500.0);  // +100 bps
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Long, 49900.0);  // -20 bps
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Long, 50300.0);

    j.on_exit_filled(Symbol("BTCUSDT"), PositionSide::Long,
                      50300.0, /*gross_pnl=*/3.0, /*fees=*/0.30,
                      ExitLayer::ExchangeTP, "tp_hit");

    auto rows = j.closed_rows();
    REQUIRE(rows.size() == 1);
    const auto& r = rows.front();
    REQUIRE(r.signal_age_ms == 80);
    REQUIRE(r.hold_duration_ms == 600'000);
    REQUIRE(r.entry_price == 50000.0);
    REQUIRE(r.exit_price == 50300.0);
    REQUIRE(r.net_pnl == 2.70);
    REQUIRE(r.exit_layer == ExitLayer::ExchangeTP);
    // MFE = (50500 - 50000) / 50000 * 10000 = 100 bps
    REQUIRE_THAT(r.mfe_bps, WithinAbs(100.0, 1e-6));
    // MAE = (50000 - 49900) / 50000 * 10000 = 20 bps
    REQUIRE_THAT(r.mae_bps, WithinAbs(20.0, 1e-6));
    // net_pnl_bps = (2.70 / (50000*0.01)) * 10000 = 54 bps
    // giveback = mfe - net_pnl_bps = 100 - 54 = 46 bps
    REQUIRE_THAT(r.giveback_bps, WithinAbs(46.0, 0.5));
}

TEST_CASE("TradeJournal: SHORT position — favorable направления вниз",
          "[trade_journal][edge-31]") {
    auto log = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<FakeClock>();
    clk->set(1'000'000'000'000LL);

    TradeJournal j(log, clk);
    j.on_entry_filled(Symbol("BTCUSDT"), PositionSide::Short,
                       50000.0, 0.01, clk->now().get() - 50'000'000,
                       "scalp", "Rejection", 0.7);

    // SHORT: цена вниз = favorable.
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Short, 49500.0);  // -100 bps = +100 favorable
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Short, 50200.0);  // +40 bps adverse
    j.on_tick(Symbol("BTCUSDT"), PositionSide::Short, 49600.0);

    clk->advance(60'000'000'000LL);  // +60s
    j.on_exit_filled(Symbol("BTCUSDT"), PositionSide::Short,
                      49600.0, 2.0, 0.20, ExitLayer::ExchangeSL, "sl_trail");

    auto rows = j.closed_rows();
    REQUIRE(rows.size() == 1);
    const auto& r = rows.front();
    REQUIRE_THAT(r.mfe_bps, WithinAbs(100.0, 1e-6));  // 50000→49500 = 100 bps favorable for short
    REQUIRE_THAT(r.mae_bps, WithinAbs(40.0, 1e-6));   // 50000→50200 = 40 bps adverse for short
    REQUIRE(r.exit_layer == ExitLayer::ExchangeSL);
}

TEST_CASE("TradeJournal: exit без entry → пропуск (warn только)",
          "[trade_journal][edge-31]") {
    auto log = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<FakeClock>();
    TradeJournal j(log, clk);

    j.on_exit_filled(Symbol("ETHUSDT"), PositionSide::Long, 3000.0, 0.0, 0.0,
                      ExitLayer::Manual, "orphan_close");
    REQUIRE(j.closed_rows().empty());
}

TEST_CASE("TradeJournal: множественные позиции в разных symbol/side изолированы",
          "[trade_journal][edge-31]") {
    auto log = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<FakeClock>();
    clk->set(1'000'000'000'000LL);
    TradeJournal j(log, clk);

    j.on_entry_filled(Symbol("BTCUSDT"), PositionSide::Long, 50000.0, 0.01,
                       clk->now().get(), "a", "Momentum", 0.8);
    j.on_entry_filled(Symbol("ETHUSDT"), PositionSide::Short, 3000.0, 0.1,
                       clk->now().get(), "b", "Rejection", 0.6);

    REQUIRE(j.get_in_flight(Symbol("BTCUSDT"), PositionSide::Long).has_value());
    REQUIRE(j.get_in_flight(Symbol("ETHUSDT"), PositionSide::Short).has_value());

    clk->advance(30'000'000'000LL);
    j.on_exit_filled(Symbol("BTCUSDT"), PositionSide::Long, 50100.0, 1.0, 0.1,
                      ExitLayer::TrailingSL, "trail");

    // BTC закрыт, ETH ещё в полёте.
    REQUIRE_FALSE(j.get_in_flight(Symbol("BTCUSDT"), PositionSide::Long).has_value());
    REQUIRE(j.get_in_flight(Symbol("ETHUSDT"), PositionSide::Short).has_value());
    REQUIRE(j.closed_rows().size() == 1);
}

TEST_CASE("TradeJournal: clear_closed очищает буфер",
          "[trade_journal][edge-31]") {
    auto log = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<FakeClock>();
    TradeJournal j(log, clk);

    j.on_entry_filled(Symbol("BTCUSDT"), PositionSide::Long, 50000.0, 0.01,
                       clk->now().get(), "", "", 0.5);
    j.on_exit_filled(Symbol("BTCUSDT"), PositionSide::Long, 50100.0, 1.0, 0.1,
                      ExitLayer::ExchangeTP, "");
    REQUIRE(j.closed_rows().size() == 1);
    j.clear_closed();
    REQUIRE(j.closed_rows().empty());
}
