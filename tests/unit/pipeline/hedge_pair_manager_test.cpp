/**
 * @file hedge_pair_manager_test.cpp
 * @brief Unit tests for HedgePairManager (Phase 7)
 *
 * Scenarios: hedge open triggers, asymmetric unwind, profit lock,
 * emergency flatten, controlled reverse, can_hedge limit.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pipeline/hedge_pair_manager.hpp"
#include "logging/logger.hpp"

using namespace tb;
using namespace tb::pipeline;

namespace {

HedgePairInput make_base_input() {
    HedgePairInput in;
    in.primary_side = PositionSide::Long;
    in.primary_size = 0.01;
    in.primary_pnl = -5.0;
    in.primary_pnl_pct = -0.83;
    in.primary_hold_ns = 300'000'000'000LL;  // 5 min
    in.has_hedge = false;
    in.regime_stability = 0.5;
    in.regime_confidence = 0.5;
    in.cusum_regime_change = false;
    in.uncertainty = 0.4;
    in.exit_score_primary = -0.1;
    in.exit_score_hedge = 0.0;
    in.funding_rate = 0.0001;
    in.atr = 200.0;
    in.spread_bps = 2.0;
    in.depth_usd = 30000.0;
    in.vpin_toxic = false;
    in.momentum = -0.01;
    in.momentum_valid = true;
    in.total_capital = 10000.0;
    in.mid_price = 60000.0;
    in.taker_fee_pct = 0.06;
    in.hedge_trigger_loss_pct = 1.5;
    in.protective_stop_imminent = false;
    return in;
}

} // namespace

TEST_CASE("HedgePairManager: initial state is PrimaryOnly", "[hedge]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    REQUIRE(mgr.state() == HedgePairState::PrimaryOnly);
    REQUIRE(mgr.can_hedge());
    REQUIRE(mgr.hedge_count() == 0);
}

TEST_CASE("HedgePairManager: regime break triggers hedge open", "[hedge][trigger]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    auto input = make_base_input();
    input.cusum_regime_change = true;
    input.regime_stability = 0.2;
    input.primary_pnl = -200.0;   // 2.0% of capital (exceeds hedge_trigger_loss_pct=1.5%)
    input.primary_pnl_pct = -2.0;

    auto decision = mgr.evaluate(input);

    REQUIRE(decision.action == HedgeAction::OpenHedge);
    CHECK(decision.hedge_ratio >= 0.3);
    CHECK(decision.hedge_ratio <= 1.2);
    CHECK(!decision.reason.empty());
}

TEST_CASE("HedgePairManager: toxic flow + loss triggers hedge", "[hedge][trigger]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    auto input = make_base_input();
    input.vpin_toxic = true;
    input.primary_pnl_pct = -2.0;  // Losing position
    input.primary_pnl = -200.0;    // 2.0% of capital (exceeds hedge_trigger_loss_pct=1.5%)

    auto decision = mgr.evaluate(input);

    // Toxic flow + loss should trigger hedge
    CHECK(decision.action == HedgeAction::OpenHedge);
}

TEST_CASE("HedgePairManager: imminent protective stop triggers hedge before plain stop-out", "[hedge][trigger]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    auto input = make_base_input();
    input.hedge_trigger_loss_pct = 4.0;  // Production-style threshold: stop pressure must bypass it
    input.primary_pnl = -175.0;          // 1.75% of capital, below configured hedge trigger
    input.primary_pnl_pct = -1.75;
    input.exit_score_primary = -0.08;    // soft but adverse trend
    input.momentum = -0.012;
    input.momentum_valid = true;
    input.protective_stop_imminent = true;
    input.stop_distance_pct = 0.06;

    auto decision = mgr.evaluate(input);

    REQUIRE(decision.action == HedgeAction::OpenHedge);
    CHECK(decision.hedge_ratio >= 1.0);
}

TEST_CASE("HedgePairManager: pair with net profit → close both", "[hedge][profit_lock]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    // First trigger a hedge
    auto input = make_base_input();
    input.cusum_regime_change = true;
    input.regime_stability = 0.2;
    input.primary_pnl = -200.0;
    input.primary_pnl_pct = -2.0;
    mgr.evaluate(input);
    mgr.notify_hedge_opened();

    REQUIRE(mgr.state() == HedgePairState::PrimaryPlusHedge);

    // Now hedge is profitable enough to cover whole pair
    input.has_hedge = true;
    input.hedge_size = 0.01;
    input.hedge_pnl = 15.0;
    input.hedge_pnl_pct = 2.5;
    input.hedge_hold_ns = 180'000'000'000LL;  // 3 min
    input.cusum_regime_change = false;
    input.regime_stability = 0.5;
    input.primary_pnl = -10.0;    // Primary recovered from -200 to -10
    input.primary_pnl_pct = -1.67;

    auto decision = mgr.evaluate(input);

    // Net PnL = -10 + 15 = 5 > fees → should close both
    CHECK((decision.action == HedgeAction::CloseBoth ||
           decision.action == HedgeAction::CloseHedge));
}

TEST_CASE("HedgePairManager: asymmetric unwind of winning hedge", "[hedge][asymmetric]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    // Get into hedged state
    auto input = make_base_input();
    input.cusum_regime_change = true;
    input.regime_stability = 0.15;
    input.primary_pnl = -200.0;
    input.primary_pnl_pct = -2.0;
    mgr.evaluate(input);
    mgr.notify_hedge_opened();

    // Hedge leg becomes profitable, primary still losing
    input.has_hedge = true;
    input.hedge_size = 0.01;
    input.hedge_pnl = 8.0;   // > round-trip fees
    input.hedge_pnl_pct = 1.33;
    input.hedge_hold_ns = 60'000'000'000LL;  // 1 min > min hold
    input.primary_pnl = -15.0;
    input.primary_pnl_pct = -2.5;
    input.cusum_regime_change = false;
    input.regime_stability = 0.6;  // Regime stabilized → triggers asymmetric unwind

    auto decision = mgr.evaluate(input);

    // Asymmetric unwind: close hedge leg, keep primary
    // or close primary and reverse
    CHECK((decision.action == HedgeAction::CloseHedge ||
           decision.action == HedgeAction::ClosePrimary ||
           decision.action == HedgeAction::CloseBoth));
}

TEST_CASE("HedgePairManager: can_hedge respects max hedges", "[hedge][limit]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    REQUIRE(mgr.can_hedge());

    // Simulate 2 hedge cycles
    mgr.notify_hedge_opened();
    mgr.notify_hedge_closed();
    mgr.notify_hedge_opened();
    mgr.notify_hedge_closed();

    // After kMaxHedgesPerPosition=2 hedges, can't hedge again
    CHECK_FALSE(mgr.can_hedge());
}

TEST_CASE("HedgePairManager: reset clears state", "[hedge][reset]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    mgr.notify_hedge_opened();
    REQUIRE(mgr.state() == HedgePairState::PrimaryPlusHedge);
    REQUIRE(mgr.hedge_count() == 1);

    mgr.reset();

    CHECK(mgr.state() == HedgePairState::PrimaryOnly);
    CHECK(mgr.hedge_count() == 0);
    CHECK(mgr.can_hedge());
}

TEST_CASE("HedgePairManager: reverse transition", "[hedge][reverse]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    mgr.notify_hedge_opened();
    REQUIRE(mgr.state() == HedgePairState::PrimaryPlusHedge);

    mgr.notify_reversed();
    CHECK(mgr.state() == HedgePairState::PrimaryOnly);
}

TEST_CASE("HedgePairManager: no hedge on healthy primary", "[hedge][no_trigger]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    HedgePairManager mgr(logger);

    auto input = make_base_input();
    // Healthy position: no regime break, no toxic flow, small loss
    input.cusum_regime_change = false;
    input.regime_stability = 0.7;
    input.vpin_toxic = false;
    input.primary_pnl_pct = -0.3;
    input.depth_usd = 50000.0;

    auto decision = mgr.evaluate(input);

    CHECK(decision.action == HedgeAction::None);
}
