/**
 * @file exit_orchestrator_test.cpp
 * @brief Unit tests for PositionExitOrchestrator (edge-31 Phase 5 — simplified).
 *
 * После edge-31 Phase 5 orchestrator отвечает ТОЛЬКО за emergency-tier exits:
 *   - check_fixed_capital_stop (hard capital cap)
 *   - check_toxic_flow (toxic микроструктура)
 *   - check_stale_data_exit (feed протух)
 *
 * Все "alpha-tier" exits (continuation_value, structural_failure,
 * market_regime_exit, liquidity_deterioration, partial_tp, quick_profit,
 * funding_carry, price_stop, force_tp_high, fast_adverse_tiered) удалены.
 * Их функцию закрывает exchange-attached TP/SL + Phase 4 trailing bracket push.
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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Healthy position — orchestrator не должен инициировать exit.
// Защита целиком на exchange-attached TP/SL.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: healthy position → no emergency exit",
          "[exit][edge-31]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.current_price = 60100.0;
    ctx.mid_price = 60100.0;
    ctx.unrealized_pnl = 1.0;
    ctx.unrealized_pnl_pct = 0.17;
    auto decision = orch.evaluate(ctx);

    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

// ─────────────────────────────────────────────────────────────────────────────
// Profitable position не закрывается локально — exchange TP делает это.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: large profit не закрывается локально (TP на бирже)",
          "[exit][edge-31]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.entry_price = 100.0;
    ctx.current_price = 101.50;  // +1.5% gross profit
    ctx.mid_price = 101.50;
    ctx.position_size = 1.0;
    ctx.unrealized_pnl = 1.50;
    ctx.unrealized_pnl_pct = 1.50;
    ctx.highest_price_since_entry = 101.50;

    auto decision = orch.evaluate(ctx);

    // Orchestrator больше не закрывает на profit — это работа exchange TP.
    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hard capital stop — safety net на случай отказа exchange SL.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: hard capital stop triggers on capital breach",
          "[exit][edge-31][risk_kill]") {
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

// ─────────────────────────────────────────────────────────────────────────────
// Toxic flow — emergency. VPIN toxic + meaningful loss → exit.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: toxic flow triggers emergency exit",
          "[exit][edge-31][toxic]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.vpin_toxic = true;
    ctx.book_imbalance = -0.8;
    ctx.buy_pressure = 0.15;
    // pnl must satisfy EDGE-24 thresholds: -0.3% min, и hold>5min для меньшей строгости.
    // Используем hold > 5min с pnl_pct = -0.4%, чтобы capital_stop не сработал первым.
    ctx.entry_time_ns = 0;
    ctx.now_ns = 600'000'000'000LL;  // 600s = 10 min hold
    ctx.unrealized_pnl = -3.5;
    ctx.unrealized_pnl_pct = -0.4;
    ctx.current_price = 59760.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    REQUIRE(decision.explanation.primary_signal == ExitSignalType::ToxicFlowExit);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stale data — emergency. Feed not fresh → safety exit.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: stale data triggers safety exit",
          "[exit][edge-31][stale]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.is_feed_fresh = false;
    ctx.spread_bps = 50.0;
    ctx.depth_usd = 500.0;

    auto decision = orch.evaluate(ctx);

    REQUIRE(decision.should_exit);
    REQUIRE(decision.explanation.primary_signal == ExitSignalType::StaleDataExit);
    REQUIRE(exit_category(decision.explanation.primary_signal) == ExitCategory::StaleData);
}

// ─────────────────────────────────────────────────────────────────────────────
// Regime change БЕЗ capital breach — orchestrator больше не реагирует.
// Эту функцию теперь несёт сигнал стратегии (signal-based exit intent).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: regime change без capital breach НЕ инициирует exit",
          "[exit][edge-31]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.cusum_regime_change = true;
    ctx.regime_stability = 0.15;
    ctx.regime_confidence = 0.3;
    ctx.current_price = 59800.0;
    ctx.unrealized_pnl = -2.0;
    ctx.unrealized_pnl_pct = -0.33;
    ctx.ema_20 = 59850.0;
    ctx.ema_50 = 59950.0;
    ctx.macd_histogram = -3.0;

    auto decision = orch.evaluate(ctx);

    // edge-31 Phase 5: regime exit удалён — orchestrator пропускает.
    REQUIRE_FALSE(decision.should_exit);
    REQUIRE_FALSE(decision.should_reduce);
}

// ─────────────────────────────────────────────────────────────────────────────
// Trailing — update_trailing вычисляет новый SL уровень.
// (Закрытия НЕ инициирует — это делает bracket SL на бирже после Phase 4 push.)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("ExitOrchestrator: update_trailing считает новый SL уровень для bracket push",
          "[exit][edge-31][trailing]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    PositionExitOrchestrator orch(logger, clk);

    auto ctx = make_base_context();
    ctx.entry_price = 60000.0;
    ctx.highest_price_since_entry = 60500.0;
    ctx.current_price = 60450.0;
    ctx.atr_14 = 200.0;
    ctx.breakeven_activated = true;
    ctx.unrealized_pnl_pct = 0.75;

    auto u = orch.update_trailing(ctx);

    // SL должен быть выше начального для long с peak'ом наверху.
    REQUIRE(u.stop_level > 0.0);
    REQUIRE(u.highest == 60500.0);
    REQUIRE(u.breakeven_activated);
}
