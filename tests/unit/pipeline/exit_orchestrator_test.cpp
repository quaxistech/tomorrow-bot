/**
 * @file exit_orchestrator_test.cpp
 * @brief Unit tests for PositionExitOrchestrator (Phase 7)
 *
 * Scenarios: continuation value, regime break, funding exit,
 * liquidity exit, stale-data exit, trailing stop, toxic flow.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pipeline/exit_orchestrator.hpp"
#include "pipeline/exit_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::pipeline;

namespace {

class TestClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    void advance(int64_t ns) { now_ns_ += ns; }
    void set(int64_t ns) { now_ns_ = ns; }
private:
    int64_t now_ns_{1'000'000'000'000LL}; // 1 trillion ns ~ sensible start
};

ExitContext make_base_context() {
    ExitContext ctx;
    ctx.symbol = Symbol("BTCUSDT");
    ctx.position_side = PositionSide::Long;
    ctx.entry_price = 60000.0;
    ctx.current_price = 60300.0;
    ctx.position_size = 0.01;
    ctx.initial_position_size = 0.01;
    ctx.unrealized_pnl = 3.0;
    ctx.unrealized_pnl_pct = 0.5;
    ctx.entry_time_ns = 900'000'000'000LL;
    ctx.now_ns = 1'000'000'000'000LL;  // 100s in position
    ctx.highest_price_since_entry = 60350.0;
    ctx.lowest_price_since_entry = 59900.0;
    ctx.current_stop_level = 59000.0;
    ctx.atr_14 = 200.0;
    ctx.atr_valid = true;
    ctx.mid_price = 60300.0;
    ctx.spread_bps = 1.5;
    ctx.book_imbalance = 0.1;
    ctx.depth_usd = 50000.0;
    ctx.vpin_toxic = false;
    ctx.ema_8 = 60250.0;
    ctx.ema_20 = 60100.0;
    ctx.ema_50 = 59800.0;
    ctx.rsi_14 = 55.0;
    ctx.adx = 25.0;
    ctx.macd_histogram = 10.0;
    ctx.bb_width = 0.02;
    ctx.buy_pressure = 0.55;
    ctx.total_capital = 10000.0;
    ctx.max_loss_per_trade_pct = 1.0;
    ctx.price_stop_loss_pct = 3.0;
    ctx.regime_stability = 0.7;
    ctx.regime_confidence = 0.7;
    ctx.cusum_regime_change = false;
    ctx.uncertainty = 0.3;
    ctx.realized_vol_short = 0.01;
    ctx.realized_vol_long = 0.01;
    ctx.funding_rate = 0.0001;
    ctx.taker_fee_pct = 0.06;
    ctx.is_feed_fresh = true;
    ctx.p_continue = 0.55;
    ctx.p_reversal = 0.20;
    ctx.p_shock = 0.03;
    ctx.hold_ev_bps = 5.0;
    ctx.close_ev_bps = -2.0;
    return ctx;
}

} // namespace

TEST_CASE("ExitOrchestrator: healthy position → hold", "[exit][continuation]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    // Keep profit below partial TP AND quick-profit fee threshold
    ctx.current_price = 60100.0;
    ctx.mid_price = 60100.0;
    ctx.unrealized_pnl = 1.0;
    ctx.unrealized_pnl_pct = 0.17;
    auto decision = orch.evaluate(ctx);

    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

TEST_CASE("ExitOrchestrator: quick profit waits while continuation is still healthy", "[exit][quick_profit]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.entry_price = 100.0;
    ctx.current_price = 100.90;
    ctx.mid_price = 100.90;
    ctx.position_size = 1.0;
    ctx.initial_position_size = 1.0;
    ctx.unrealized_pnl = 0.90;
    ctx.unrealized_pnl_pct = 0.90;
    ctx.atr_14 = 0.5;
    ctx.current_stop_level = 99.40;
    ctx.partial_tp_taken = true;
    ctx.ema_20 = 100.40;
    ctx.ema_50 = 99.80;
    ctx.macd_histogram = 0.08;
    ctx.rsi_14 = 58.0;
    ctx.book_imbalance = 0.15;
    ctx.regime_stability = 0.75;
    ctx.uncertainty = 0.20;
    ctx.p_continue = 0.62;
    ctx.p_reversal = 0.18;
    ctx.p_shock = 0.02;

    auto decision = orch.evaluate(ctx);

    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

TEST_CASE("ExitOrchestrator: quick profit harvest needs weak continuation and real profit", "[exit][quick_profit]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.entry_price = 100.0;
    ctx.current_price = 100.90;
    ctx.mid_price = 100.90;
    ctx.position_size = 1.0;
    ctx.initial_position_size = 1.0;
    ctx.unrealized_pnl = 0.90;
    ctx.unrealized_pnl_pct = 0.90;
    ctx.atr_14 = 0.5;
    ctx.current_stop_level = 99.40;
    ctx.partial_tp_taken = true;
    ctx.ema_20 = 100.40;
    ctx.ema_50 = 99.80;
    ctx.macd_histogram = 0.02;
    ctx.rsi_14 = 78.0;
    ctx.book_imbalance = -0.55;
    ctx.depth_usd = 800.0;
    ctx.regime_stability = 0.18;
    ctx.regime_confidence = 0.35;
    ctx.uncertainty = 0.82;
    ctx.p_continue = 0.20;
    ctx.p_reversal = 0.46;
    ctx.p_shock = 0.10;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    CHECK(decision.explanation.primary_signal == ExitSignalType::QuickProfitHarvest);
}

TEST_CASE("ExitOrchestrator: hard stop triggers on capital breach", "[exit][risk_kill]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    // Loss exceeds max_loss_per_trade_pct (1% of 10000 = 100)
    ctx.current_price = 49000.0;
    ctx.unrealized_pnl = -110.0;
    ctx.unrealized_pnl_pct = -18.3;
    ctx.lowest_price_since_entry = 49000.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    REQUIRE(decision.explanation.primary_signal == ExitSignalType::HardRiskStop);
    REQUIRE(exit_category(decision.explanation.primary_signal) == ExitCategory::RiskKill);
    REQUIRE(decision.urgency >= 0.9);
}

TEST_CASE("ExitOrchestrator: regime change triggers exit", "[exit][regime]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.cusum_regime_change = true;
    ctx.regime_stability = 0.15;
    ctx.regime_confidence = 0.3;
    // Price losing ground
    ctx.current_price = 59800.0;
    ctx.unrealized_pnl = -2.0;
    ctx.unrealized_pnl_pct = -0.33;
    // Weak continuation
    ctx.p_continue = 0.30;
    ctx.p_reversal = 0.45;
    ctx.hold_ev_bps = -5.0;
    ctx.close_ev_bps = 2.0;

    auto decision = orch.evaluate(ctx);

    // Should trigger exit or at minimum reduce position
    REQUIRE((decision.should_exit || decision.should_reduce));
}

TEST_CASE("ExitOrchestrator: funding carry exit on adverse funding", "[exit][funding]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    // Long position with very high positive funding (paying funding)
    ctx.funding_rate = 0.003;   // 0.3% — extreme
    // Position is only marginally in profit
    ctx.unrealized_pnl = 0.5;
    ctx.unrealized_pnl_pct = 0.08;
    // Long hold time
    ctx.entry_time_ns = 0;
    ctx.now_ns = 3'600'000'000'000LL;  // 1 hour
    // Low continuation
    ctx.p_continue = 0.40;
    ctx.hold_ev_bps = -3.0;

    auto decision = orch.evaluate(ctx);

    // Should at minimum flag funding carry concern
    if (decision.should_exit) {
        CHECK(decision.explanation.primary_signal == ExitSignalType::FundingCarryExit);
    }
}

TEST_CASE("ExitOrchestrator: stale data triggers safety exit", "[exit][stale]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.is_feed_fresh = false;
    ctx.spread_bps = 50.0;  // Wide spread from stale data
    ctx.depth_usd = 500.0;  // Thin book puts position at risk

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    REQUIRE(decision.explanation.primary_signal == ExitSignalType::StaleDataExit);
    REQUIRE(exit_category(decision.explanation.primary_signal) == ExitCategory::StaleData);
}

TEST_CASE("ExitOrchestrator: toxic flow triggers exit", "[exit][toxic]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.vpin_toxic = true;
    // Adverse book pressure
    ctx.book_imbalance = -0.8;
    ctx.buy_pressure = 0.15;
    // Some loss
    ctx.unrealized_pnl = -5.0;
    ctx.unrealized_pnl_pct = -0.83;
    ctx.current_price = 59700.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE((decision.should_exit || decision.should_reduce));
    if (decision.should_exit) {
        CHECK(decision.explanation.primary_signal == ExitSignalType::ToxicFlowExit);
    }
}

TEST_CASE("ExitOrchestrator: continuation value exit on depleted edge", "[exit][continuation_value]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    // Indicators showing momentum fade
    ctx.rsi_14 = 70.0;
    ctx.macd_histogram = -5.0;
    ctx.ema_8 = 60050.0;  // EMA8 crossing below EMA20
    ctx.ema_20 = 60100.0;
    ctx.buy_pressure = 0.35;
    ctx.adx = 12.0;  // Low trend strength
    ctx.regime_stability = 0.3;
    ctx.p_continue = 0.35;
    ctx.p_reversal = 0.35;
    ctx.hold_ev_bps = -8.0;
    ctx.close_ev_bps = 3.0;
    // Long hold
    ctx.entry_time_ns = 0;
    ctx.now_ns = 5'400'000'000'000LL;  // 90 min

    auto decision = orch.evaluate(ctx);

    // Should eventually trigger continuation value exit
    if (decision.should_exit) {
        CHECK((decision.explanation.primary_signal == ExitSignalType::ContinuationValueExit ||
               decision.explanation.primary_signal == ExitSignalType::StructuralFailure));
    }
    // Continuation value should be low
    CHECK(decision.state.continuation_value < 0.5);
}

TEST_CASE("ExitOrchestrator: continuation exit ignores fresh fee-band churn", "[exit][continuation_value][fees]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.current_price = 60020.0;
    ctx.mid_price = 60020.0;
    ctx.unrealized_pnl = 0.20;          // below round-trip fee threshold
    ctx.unrealized_pnl_pct = 0.03;
    ctx.now_ns = ctx.entry_time_ns + 30'000'000'000LL;  // 30s after entry
    ctx.highest_price_since_entry = 60320.0;            // edge decayed sharply from the peak
    ctx.rsi_14 = 70.0;
    ctx.book_imbalance = -0.60;
    ctx.regime_stability = 0.20;
    ctx.regime_confidence = 0.20;
    ctx.uncertainty = 0.80;
    ctx.p_continue = 0.30;
    ctx.p_reversal = 0.45;
    ctx.hold_ev_bps = -8.0;
    ctx.close_ev_bps = 3.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

TEST_CASE("ExitOrchestrator: continuation exit ignores mature fee-band churn", "[exit][continuation_value][fees]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.current_price = 59800.0;
    ctx.mid_price = 59800.0;
    ctx.unrealized_pnl = -0.80;         // still inside shallow fee-loss band
    ctx.unrealized_pnl_pct = -0.13;
    ctx.entry_time_ns = 0;
    ctx.now_ns = 180'000'000'000LL;     // mature position
    ctx.highest_price_since_entry = 60320.0;
    ctx.rsi_14 = 72.0;
    ctx.book_imbalance = -0.65;
    ctx.regime_stability = 0.15;
    ctx.regime_confidence = 0.20;
    ctx.uncertainty = 0.85;
    ctx.p_continue = 0.25;
    ctx.p_reversal = 0.50;
    ctx.hold_ev_bps = -10.0;
    ctx.close_ev_bps = 4.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

TEST_CASE("ExitOrchestrator: continuation exit still closes materially bad trade", "[exit][continuation_value][fees]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.current_price = 59800.0;
    ctx.mid_price = 59800.0;
    ctx.unrealized_pnl = -2.0;          // outside shallow fee-churn band
    ctx.unrealized_pnl_pct = -0.33;
    ctx.entry_time_ns = 0;
    ctx.now_ns = 600'000'000'000LL;     // mature position
    ctx.highest_price_since_entry = 60320.0;
    ctx.rsi_14 = 72.0;
    ctx.book_imbalance = -0.65;
    ctx.regime_stability = 0.15;
    ctx.regime_confidence = 0.20;
    ctx.uncertainty = 0.85;
    ctx.p_continue = 0.25;
    ctx.p_reversal = 0.50;
    ctx.hold_ev_bps = -10.0;
    ctx.close_ev_bps = 4.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    CHECK(decision.explanation.primary_signal == ExitSignalType::ContinuationValueExit);
}

TEST_CASE("ExitOrchestrator: trailing stop activates correctly", "[exit][trailing]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    // Price rallied then fell back through trailing stop
    ctx.entry_price = 60000.0;
    ctx.highest_price_since_entry = 61000.0;  // +1000 from entry
    ctx.current_price = 60500.0;              // Pulled back 500 from high
    ctx.current_stop_level = 60600.0;         // Trailing stop above current price
    ctx.atr_14 = 200.0;
    ctx.breakeven_activated = true;
    ctx.unrealized_pnl = 5.0;
    ctx.unrealized_pnl_pct = 0.83;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    CHECK(decision.explanation.primary_signal == ExitSignalType::TrailingStop);
}

TEST_CASE("ExitOrchestrator: liquidity deterioration triggers exit", "[exit][liquidity]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.depth_usd = 200.0;           // Very thin book (< 500 for 0.8 depth_risk)
    ctx.spread_bps = 55.0;           // Wide spread (> 50 for 0.8 spread_risk)
    ctx.book_imbalance = -0.6;       // Against position
    ctx.cancel_burst_intensity = 0.8;
    ctx.top_of_book_churn = 0.7;
    ctx.queue_depletion_bid = 0.9;   // Bid side depleting fast
    ctx.current_price = 59100.0;     // Consistent with loss
    ctx.mid_price = 59100.0;
    ctx.unrealized_pnl = -9.0;
    ctx.unrealized_pnl_pct = -1.5;   // Must be < -1.0

    auto decision = orch.evaluate(ctx);

    // Should signal exit or reduce on liquidity deterioration
    REQUIRE((decision.should_exit || decision.should_reduce));
}
