/**
 * @file test_cost_attribution.cpp
 * @brief Тесты CostAttributionEngine — декомпозиция PnL
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cost_attribution/cost_attribution_engine.hpp"

using namespace tb;
using namespace tb::cost_attribution;

TEST_CASE("CostAttribution: record and count", "[cost_attribution]") {
    CostAttributionEngine engine;
    REQUIRE(engine.trade_count() == 0);

    TradeCostBreakdown trade;
    trade.trade_id = "t1";
    trade.gross_pnl_usdt = 100.0;
    trade.total_fee_usdt = 2.0;
    trade.slippage_usdt = 1.0;
    trade.funding_cost_usdt = 0.5;
    trade.missed_fill_cost_usdt = 0.0;
    engine.record_trade(std::move(trade));

    REQUIRE(engine.trade_count() == 1);
}

TEST_CASE("CostAttribution: net PnL computed on record", "[cost_attribution]") {
    CostAttributionEngine engine;

    TradeCostBreakdown trade;
    trade.gross_pnl_usdt = 100.0;
    trade.total_fee_usdt = 5.0;
    trade.slippage_usdt = 3.0;
    trade.funding_cost_usdt = 2.0;
    trade.missed_fill_cost_usdt = 1.0;
    engine.record_trade(std::move(trade));

    auto summary = engine.summarize_all();
    REQUIRE(summary.trade_count == 1);
    REQUIRE_THAT(summary.total_net_pnl, Catch::Matchers::WithinAbs(89.0, 0.01));
}

TEST_CASE("CostAttribution: summary aggregates correctly", "[cost_attribution]") {
    CostAttributionEngine engine;

    for (int i = 0; i < 10; ++i) {
        TradeCostBreakdown t;
        t.trade_id = "t" + std::to_string(i);
        t.symbol = Symbol{"BTCUSDT"};
        t.opened_at = Timestamp{static_cast<int64_t>(i * 1000)};
        t.closed_at = Timestamp{static_cast<int64_t>(i * 1000 + 500)};
        t.gross_pnl_usdt = 50.0;
        t.total_fee_usdt = 2.0;
        t.slippage_usdt = 1.0;
        t.slippage_bps = 5.0;
        t.total_cost_bps = 15.0;
        engine.record_trade(std::move(t));
    }

    auto summary = engine.summarize_all();
    REQUIRE(summary.trade_count == 10);
    REQUIRE_THAT(summary.total_gross_pnl, Catch::Matchers::WithinAbs(500.0, 0.01));
    REQUIRE_THAT(summary.total_fees, Catch::Matchers::WithinAbs(20.0, 0.01));
    REQUIRE_THAT(summary.total_slippage, Catch::Matchers::WithinAbs(10.0, 0.01));
    REQUIRE_THAT(summary.avg_slippage_bps, Catch::Matchers::WithinAbs(5.0, 0.01));
    REQUIRE_THAT(summary.avg_total_cost_bps, Catch::Matchers::WithinAbs(15.0, 0.01));

    // Percentages of gross
    REQUIRE(summary.fees_pct > 0.0);
    REQUIRE(summary.slippage_pct > 0.0);
}

TEST_CASE("CostAttribution: time-filtered summarize", "[cost_attribution]") {
    CostAttributionEngine engine;

    TradeCostBreakdown t1;
    t1.opened_at = Timestamp{1000};
    t1.closed_at = Timestamp{2000};
    t1.gross_pnl_usdt = 100.0;
    engine.record_trade(std::move(t1));

    TradeCostBreakdown t2;
    t2.opened_at = Timestamp{5000};
    t2.closed_at = Timestamp{6000};
    t2.gross_pnl_usdt = 200.0;
    engine.record_trade(std::move(t2));

    auto summary = engine.summarize(Timestamp{4000}, Timestamp{7000});
    REQUIRE(summary.trade_count == 1);
    REQUIRE_THAT(summary.total_gross_pnl, Catch::Matchers::WithinAbs(200.0, 0.01));
}

TEST_CASE("CostAttribution: clear resets state", "[cost_attribution]") {
    CostAttributionEngine engine;

    TradeCostBreakdown t;
    t.gross_pnl_usdt = 50.0;
    engine.record_trade(std::move(t));
    REQUIRE(engine.trade_count() == 1);

    engine.clear();
    REQUIRE(engine.trade_count() == 0);
}

TEST_CASE("CostAttribution: empty summary is valid", "[cost_attribution]") {
    CostAttributionEngine engine;
    auto summary = engine.summarize_all();
    REQUIRE(summary.trade_count == 0);
    REQUIRE_THAT(summary.total_gross_pnl, Catch::Matchers::WithinAbs(0.0, 0.01));
}
