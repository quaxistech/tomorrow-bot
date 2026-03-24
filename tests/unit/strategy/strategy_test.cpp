#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "strategy/momentum/momentum_strategy.hpp"
#include "strategy/mean_reversion/mean_reversion_strategy.hpp"
#include "strategy/breakout/breakout_strategy.hpp"
#include "strategy/microstructure_scalp/microstructure_scalp_strategy.hpp"
#include "strategy/vol_expansion/vol_expansion_strategy.hpp"
#include "strategy/strategy_registry.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::strategy;
using namespace tb::features;

namespace {

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

StrategyContext make_context() {
    StrategyContext ctx;
    ctx.features.symbol = Symbol("BTCUSDT");
    ctx.features.computed_at = Timestamp(1000000);
    ctx.features.last_price = Price(50000.0);
    ctx.features.mid_price = Price(50000.0);
    ctx.features.book_quality = order_book::BookQuality::Valid;
    ctx.is_strategy_enabled = true;
    ctx.strategy_weight = 1.0;
    return ctx;
}

} // anonymous namespace

// ============================================================
// Momentum
// ============================================================

TEST_CASE("Momentum: восходящий тренд → BUY с conviction > 0", "[strategy][momentum]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    MomentumStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 51000.0;
    ctx.features.technical.ema_50 = 49000.0;
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.rsi_14 = 55.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 30.0;
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.momentum_5 = 0.02;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->conviction > 0.0);
    REQUIRE(result->strategy_id.get() == "momentum");
}

TEST_CASE("Momentum: нет тренда (низкий ADX) → нет сигнала", "[strategy][momentum]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    MomentumStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 51000.0;
    ctx.features.technical.ema_50 = 49000.0;
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.rsi_14 = 55.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 15.0; // Слишком низкий
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.momentum_5 = 0.02;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Mean Reversion
// ============================================================

TEST_CASE("MeanReversion: перепроданность → BUY", "[strategy][mean_reversion]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    MeanReversionStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.last_price = Price(48000.0);
    ctx.features.technical.bb_lower = 49000.0;
    ctx.features.technical.bb_upper = 52000.0;
    ctx.features.technical.bb_middle = 50500.0;
    ctx.features.technical.bb_bandwidth = 0.04;
    ctx.features.technical.bb_valid = true;
    ctx.features.technical.rsi_14 = 25.0;
    ctx.features.technical.rsi_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->conviction > 0.0);
}

// ============================================================
// Breakout
// ============================================================

TEST_CASE("Breakout: сжатие → расширение + пробой → BUY", "[strategy][breakout]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    BreakoutStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.bb_valid = true;
    ctx.features.technical.bb_upper = 51000.0;
    ctx.features.technical.bb_lower = 49000.0;

    // Подаём серию снимков с узким BB bandwidth (сжатие)
    for (int i = 0; i < 5; ++i) {
        ctx.features.technical.bb_bandwidth = 0.01; // Узкий
        ctx.features.last_price = Price(50000.0);
        strategy.evaluate(ctx);
    }

    // Расширение + пробой вверх
    ctx.features.technical.bb_bandwidth = 0.05; // Расширился > 1.5x от min
    ctx.features.last_price = Price(52000.0); // Выше верхней BB

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->conviction > 0.0);
}

// ============================================================
// Microstructure Scalp
// ============================================================

TEST_CASE("MicrostructureScalp: сильный дисбаланс → BUY", "[strategy][microstructure_scalp]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    MicrostructureScalpStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.microstructure.book_imbalance_5 = 0.5;
    ctx.features.microstructure.book_imbalance_valid = true;
    ctx.features.microstructure.buy_sell_ratio = 2.0;
    ctx.features.microstructure.trade_flow_valid = true;
    ctx.features.microstructure.spread_bps = 5.0;
    ctx.features.microstructure.spread_valid = true;
    ctx.features.microstructure.mid_price = 50000.0;
    ctx.features.microstructure.spread = 5.0;

    // MicroScalp требует валидных индикаторов для контекста
    ctx.features.technical.sma_20 = 50000.0;
    ctx.features.technical.sma_valid = true;
    ctx.features.technical.rsi_14 = 55.0;  // Нейтральная зона
    ctx.features.technical.rsi_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->conviction > 0.0);
}

// ============================================================
// Vol Expansion
// ============================================================

TEST_CASE("VolExpansion: ATR растёт → сигнал", "[strategy][vol_expansion]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VolExpansionStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.atr_valid = true;
    ctx.features.technical.rsi_14 = 50.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.momentum_5 = 0.02;
    ctx.features.technical.momentum_valid = true;
    ctx.features.technical.adx = 25.0;
    ctx.features.technical.adx_valid = true;

    // Серия с низким ATR
    for (int i = 0; i < 5; ++i) {
        ctx.features.technical.atr_14 = 100.0;
        strategy.evaluate(ctx);
    }

    // ATR резко расширился (> 50%)
    ctx.features.technical.atr_14 = 200.0;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy); // Положительный momentum
    REQUIRE(result->conviction > 0.0);
}

// ============================================================
// Деактивированная стратегия
// ============================================================

TEST_CASE("Деактивированная стратегия → нет сигнала", "[strategy]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    MomentumStrategy strategy(logger, clk);

    strategy.set_active(false);
    REQUIRE_FALSE(strategy.is_active());

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 51000.0;
    ctx.features.technical.ema_50 = 49000.0;
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.rsi_14 = 55.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 30.0;
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.momentum_5 = 0.02;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Strategy Registry
// ============================================================

TEST_CASE("StrategyRegistry: регистрация и доступ", "[strategy][registry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    StrategyRegistry registry;
    registry.register_strategy(std::make_shared<MomentumStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<MeanReversionStrategy>(logger, clk));

    REQUIRE(registry.count() == 2);
    REQUIRE(registry.all().size() == 2);
    REQUIRE(registry.get(StrategyId("momentum")) != nullptr);
    REQUIRE(registry.get(StrategyId("nonexistent")) == nullptr);

    registry.unregister(StrategyId("momentum"));
    REQUIRE(registry.count() == 1);
}
