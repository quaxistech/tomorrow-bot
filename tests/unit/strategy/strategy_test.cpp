#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_mocks.hpp"
#include "strategy/strategy_engine.hpp"
#include "strategy/strategy_registry.hpp"
#include <memory>

using namespace tb;
using namespace tb::test;
using namespace tb::strategy;
using namespace tb::features;

namespace {

StrategyContext make_context() {
    StrategyContext ctx;
    ctx.features.symbol = Symbol("BTCUSDT");
    ctx.features.computed_at = Timestamp(1000000);
    ctx.features.last_price = Price(50000.0);
    ctx.features.mid_price = Price(50000.0);
    ctx.features.book_quality = order_book::BookQuality::Valid;
    ctx.is_strategy_enabled = true;
    ctx.strategy_weight = 1.0;
    ctx.data_fresh = true;
    ctx.exchange_ok = true;
    ctx.futures_enabled = true;

    // Микроструктура валидная
    ctx.features.microstructure.spread_bps = 5.0;
    ctx.features.microstructure.spread_valid = true;
    ctx.features.microstructure.spread = 5.0;
    ctx.features.microstructure.mid_price = 50000.0;
    ctx.features.microstructure.book_imbalance_valid = true;
    ctx.features.microstructure.trade_flow_valid = true;
    ctx.features.microstructure.liquidity_valid = true;
    ctx.features.microstructure.liquidity_ratio = 0.8;

    // Технические индикаторы
    ctx.features.technical.sma_20 = 50000.0;
    ctx.features.technical.sma_valid = true;
    ctx.features.technical.ema_20 = 50100.0;
    ctx.features.technical.ema_50 = 49900.0;
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.rsi_14 = 55.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 30.0;
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.atr_14 = 200.0;
    ctx.features.technical.atr_valid = true;
    ctx.features.technical.bb_percent_b = 0.5;
    ctx.features.technical.bb_valid = true;
    ctx.features.technical.momentum_5 = 0.002;
    ctx.features.technical.momentum_20 = 0.001;
    ctx.features.technical.momentum_valid = true;
    ctx.features.technical.volatility_5 = 0.01;
    ctx.features.technical.volatility_20 = 0.008;
    ctx.features.technical.volatility_valid = true;

    ctx.features.execution_context.is_feed_fresh = true;

    return ctx;
}

} // anonymous namespace

// ============================================================
// Strategy Engine: Momentum Continuation Scalp
// ============================================================

TEST_CASE("StrategyEngine: сильный дисбаланс -> setup detected", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    StrategyEngine engine(logger, clk);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    // Первый вызов — обнаружение сетапа (переход Idle → SetupForming)
    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());  // Ещё ждём подтверждения
    REQUIRE(engine.current_state() == SymbolState::SetupForming);
}

TEST_CASE("StrategyEngine: setup confirms and generates entry", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    ScalpStrategyConfig cfg;
    cfg.setup_confirmation_window_ms = 0;  // Мгновенное подтверждение для теста
    StrategyEngine engine(logger, clk, cfg);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    // Первый вызов: detect + confirm (confirmation_window=0)
    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());

    // Advance time
    clk->current_time += 1'000'000'000LL;

    // Второй вызов: validate + confirm + entry
    result = engine.evaluate(ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->conviction > 0.4);
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
    REQUIRE(result->signal_type == StrategySignalType::EnterLong);
    REQUIRE(!result->setup_id.empty());
    REQUIRE(!result->reason_codes.empty());
}

TEST_CASE("StrategyEngine: SELL signal with futures", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    ScalpStrategyConfig cfg;
    cfg.setup_confirmation_window_ms = 0;
    StrategyEngine engine(logger, clk, cfg);

    auto ctx = make_context();
    ctx.futures_enabled = true;
    // Сильный SELL дисбаланс
    ctx.features.microstructure.book_imbalance_5 = -0.5;
    ctx.features.microstructure.buy_sell_ratio = 0.5;
    ctx.features.technical.ema_20 = 49900.0;
    ctx.features.technical.ema_50 = 50100.0;
    ctx.features.technical.momentum_5 = -0.005;

    engine.evaluate(ctx);
    clk->current_time += 1'000'000'000LL;
    auto result = engine.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Sell);
    REQUIRE(result->signal_intent == SignalIntent::ShortEntry);
}

// ============================================================
// Деактивированная стратегия
// ============================================================

TEST_CASE("StrategyEngine: deactivated -> no signal", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    StrategyEngine engine(logger, clk);

    engine.set_active(false);
    REQUIRE_FALSE(engine.is_active());

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Stale data -> no entry
// ============================================================

TEST_CASE("StrategyEngine: stale data -> no entry", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    StrategyEngine engine(logger, clk);

    auto ctx = make_context();
    ctx.data_fresh = false;
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Emergency halt -> emergency exit from position
// ============================================================

TEST_CASE("StrategyEngine: emergency halt exits position", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    StrategyEngine engine(logger, clk);

    // Имитируем открытую позицию
    engine.notify_position_opened(50000.0, 0.1, Side::Buy, PositionSide::Long);

    auto ctx = make_context();
    ctx.position.has_position = true;
    ctx.position.side = Side::Buy;
    ctx.position.position_side = PositionSide::Long;
    ctx.position.size = 0.1;
    ctx.position.avg_entry_price = 50000.0;
    ctx.risk.emergency_halt = true;

    auto result = engine.evaluate(ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->signal_intent == SignalIntent::LongExit);
    REQUIRE(result->urgency == 1.0);
}

// ============================================================
// Setup cancellation on spread degradation
// ============================================================

TEST_CASE("StrategyEngine: setup cancelled on spread degradation", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    StrategyEngine engine(logger, clk);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    // Detect setup
    engine.evaluate(ctx);
    REQUIRE(engine.current_state() == SymbolState::SetupForming);

    // Ухудшение спреда
    ctx.features.microstructure.spread_bps = 50.0;  // > max * 1.5
    clk->current_time += 1'000'000'000LL;
    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(engine.current_state() == SymbolState::Cooldown);
}

// ============================================================
// Position management: pipeline owns time stop
// ============================================================

TEST_CASE("StrategyEngine: strategy layer does not emit time stop", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    ScalpStrategyConfig cfg;
    // max_hold_time_ms removed — time exits centralized in PositionExitOrchestrator
    StrategyEngine engine(logger, clk, cfg);

    engine.notify_position_opened(50000.0, 0.1, Side::Buy, PositionSide::Long);

    auto ctx = make_context();
    ctx.position.has_position = true;
    ctx.position.side = Side::Buy;
    ctx.position.position_side = PositionSide::Long;
    ctx.position.size = 0.1;
    ctx.position.avg_entry_price = 50000.0;
    ctx.position.entry_time_ns = 10'000'000'000LL;

    // Advance past max_hold_time
    clk->current_time += 2'000'000'000LL;  // 2 seconds

    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("StrategyEngine: context position sync stops repeated entry signals", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    ScalpStrategyConfig cfg;
    cfg.setup_confirmation_window_ms = 0;
    StrategyEngine engine(logger, clk, cfg);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());

    clk->current_time += 1'000'000'000LL;
    result = engine.evaluate(ctx);
    REQUIRE(result.has_value());
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);

    ctx.position.has_position = true;
    ctx.position.side = Side::Buy;
    ctx.position.position_side = PositionSide::Long;
    ctx.position.size = 0.1;
    ctx.position.avg_entry_price = 50000.0;
    ctx.position.entry_time_ns = clk->current_time;

    clk->current_time += 10'000'000LL;
    result = engine.evaluate(ctx);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(engine.current_state() == SymbolState::PositionManaging);
}

// ============================================================
// Cooldown after exit
// ============================================================

TEST_CASE("StrategyEngine: cooldown after position close", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    ScalpStrategyConfig cfg;
    cfg.cooldown_after_exit_ms = 5000;
    StrategyEngine engine(logger, clk, cfg);

    engine.notify_position_closed();
    REQUIRE(engine.current_state() == SymbolState::Cooldown);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.buy_sell_ratio = 2.0;

    auto result = engine.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());

    // Advance past cooldown
    clk->current_time += 6'000'000'000LL;
    result = engine.evaluate(ctx);
    // After cooldown expires, it transitions to Idle and may detect setup
    REQUIRE(engine.current_state() != SymbolState::Cooldown);
}

// ============================================================
// Strategy Registry works with StrategyEngine  
// ============================================================

TEST_CASE("StrategyRegistry: register StrategyEngine", "[strategy][registry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    StrategyRegistry registry;
    registry.register_strategy(std::make_shared<StrategyEngine>(logger, clk));

    REQUIRE(registry.count() == 1);
    REQUIRE(registry.all().size() == 1);
    REQUIRE(registry.get(StrategyId("scalp_engine")) != nullptr);
    REQUIRE(registry.get(StrategyId("nonexistent")) == nullptr);

    registry.unregister(StrategyId("scalp_engine"));
    REQUIRE(registry.count() == 0);
}

// ============================================================
// State machine reset
// ============================================================

TEST_CASE("StrategyEngine: reset clears all state", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;
    StrategyEngine engine(logger, clk);

    // Create some state
    engine.notify_position_opened(50000.0, 0.1, Side::Buy, PositionSide::Long);
    REQUIRE(engine.current_state() == SymbolState::PositionOpen);

    engine.reset();
    REQUIRE(engine.current_state() == SymbolState::Idle);
}

// ============================================================
// Meta returns correct info
// ============================================================

TEST_CASE("StrategyEngine: meta returns scalp_engine info", "[strategy][engine]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    StrategyEngine engine(logger, clk);

    auto m = engine.meta();
    REQUIRE(m.id.get() == "scalp_engine");
    REQUIRE(m.version.get() == 2);
    REQUIRE(m.name == "ScalpEngine");
    REQUIRE(!m.required_features.empty());
}
