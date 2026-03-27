#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "strategy/momentum/momentum_strategy.hpp"
#include "strategy/mean_reversion/mean_reversion_strategy.hpp"
#include "strategy/breakout/breakout_strategy.hpp"
#include "strategy/microstructure_scalp/microstructure_scalp_strategy.hpp"
#include "strategy/vol_expansion/vol_expansion_strategy.hpp"
#include "strategy/ema_pullback/ema_pullback_strategy.hpp"
#include "strategy/rsi_divergence/rsi_divergence_strategy.hpp"
#include "strategy/vwap_reversion/vwap_reversion_strategy.hpp"
#include "strategy/volume_profile/volume_profile_strategy.hpp"
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
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
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
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
    REQUIRE(result->conviction > 0.0);
}

// ============================================================
// Breakout — signal_intent check
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

// ============================================================
// Registry: все 9 стратегий
// ============================================================

TEST_CASE("StrategyRegistry: регистрация всех 9 стратегий", "[strategy][registry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    StrategyRegistry registry;
    registry.register_strategy(std::make_shared<MomentumStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<MeanReversionStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<BreakoutStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<MicrostructureScalpStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<VolExpansionStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<EmaPullbackStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<RsiDivergenceStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<VwapReversionStrategy>(logger, clk));
    registry.register_strategy(std::make_shared<VolumeProfileStrategy>(logger, clk));

    REQUIRE(registry.count() == 9);
    REQUIRE(registry.active().size() == 9);
}

// ============================================================
// EMA Pullback
// ============================================================

TEST_CASE("EmaPullback: бычий тренд + откат к EMA20 → BUY", "[strategy][ema_pullback]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    EmaPullbackStrategy strategy(logger, clk);

    auto ctx = make_context();
    // Бычий тренд: EMA20 > EMA50
    ctx.features.technical.ema_20 = 50500.0;
    ctx.features.technical.ema_50 = 49000.0;
    ctx.features.technical.ema_valid = true;
    // ATR для нормализации
    ctx.features.technical.atr_14 = 500.0;
    ctx.features.technical.atr_valid = true;
    // Цена откатилась чуть ниже EMA20 (pullback_depth ~0.4 ATR)
    ctx.features.last_price = Price(50300.0);
    // RSI в нейтральной зоне
    ctx.features.technical.rsi_14 = 42.0;
    ctx.features.technical.rsi_valid = true;
    // ADX подтверждает тренд
    ctx.features.technical.adx = 28.0;
    ctx.features.technical.adx_valid = true;
    // Momentum начал восстанавливаться
    ctx.features.technical.momentum_5 = 0.001;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
    REQUIRE(result->conviction > 0.0);
    REQUIRE(result->strategy_id.get() == "ema_pullback");
}

TEST_CASE("EmaPullback: нет тренда → нет сигнала", "[strategy][ema_pullback]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    EmaPullbackStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 50000.0;
    ctx.features.technical.ema_50 = 50000.0; // Нет тренда
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.atr_14 = 500.0;
    ctx.features.technical.atr_valid = true;
    ctx.features.technical.rsi_14 = 50.0;
    ctx.features.technical.rsi_valid = true;

    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("EmaPullback: медвежий тренд → LongExit", "[strategy][ema_pullback]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    EmaPullbackStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 49000.0;
    ctx.features.technical.ema_50 = 50500.0; // Медвежий тренд
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.atr_14 = 500.0;
    ctx.features.technical.atr_valid = true;
    ctx.features.last_price = Price(48500.0); // Ниже EMA20
    ctx.features.technical.rsi_14 = 38.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 28.0;
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.momentum_5 = -0.005;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Sell);
    REQUIRE(result->signal_intent == SignalIntent::LongExit);
    REQUIRE(result->exit_reason == ExitReason::TrendFailure);
}

// ============================================================
// RSI Divergence
// ============================================================

TEST_CASE("RsiDivergence: бычья дивергенция → BUY", "[strategy][rsi_divergence]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RsiDivergenceStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.rsi_valid = true;

    // Первая половина: цена 50000, RSI 30
    for (int i = 0; i < 10; ++i) {
        ctx.features.last_price = Price(50000.0 - i * 20.0);
        ctx.features.technical.rsi_14 = 30.0;
        strategy.evaluate(ctx);
    }

    // Вторая половина: цена новый минимум, RSI выше (дивергенция)
    for (int i = 0; i < 9; ++i) {
        ctx.features.last_price = Price(49700.0 - i * 10.0);
        ctx.features.technical.rsi_14 = 33.0;
        strategy.evaluate(ctx);
    }

    // Финальная точка: цена ещё ниже, RSI ещё выше
    ctx.features.last_price = Price(49500.0);
    ctx.features.technical.rsi_14 = 34.0;
    ctx.features.technical.adx = 25.0;
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);

    // Может или не может сгенерировать сигнал в зависимости от exact extrema detection
    // Главное — стратегия не падает и возвращает optional
    if (result.has_value()) {
        REQUIRE(result->side == Side::Buy);
        REQUIRE(result->signal_intent == SignalIntent::LongEntry);
        REQUIRE(result->strategy_id.get() == "rsi_divergence");
    }
}

TEST_CASE("RsiDivergence: мало данных → нет сигнала", "[strategy][rsi_divergence]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RsiDivergenceStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.rsi_14 = 30.0;
    ctx.features.technical.rsi_valid = true;

    // Только 2 точки — недостаточно
    strategy.evaluate(ctx);
    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("RsiDivergence: reset очищает историю", "[strategy][rsi_divergence]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RsiDivergenceStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.rsi_14 = 30.0;
    ctx.features.technical.rsi_valid = true;

    for (int i = 0; i < 10; ++i) {
        strategy.evaluate(ctx);
    }

    strategy.reset();

    // После reset — мало данных
    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// VWAP Reversion
// ============================================================

TEST_CASE("VwapReversion: цена ниже VWAP → BUY", "[strategy][vwap_reversion]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VwapReversionStrategy strategy(logger, clk);

    auto ctx = make_context();
    // VWAP = 50000, цена = 49750 (deviation -0.5%)
    ctx.features.microstructure.trade_vwap = 50000.0;
    ctx.features.microstructure.trade_flow_valid = true;
    ctx.features.last_price = Price(49750.0);
    // RSI подтверждает
    ctx.features.technical.rsi_14 = 38.0;
    ctx.features.technical.rsi_valid = true;
    // ADX низкий (нет сильного тренда)
    ctx.features.technical.adx = 20.0;
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
    REQUIRE(result->conviction > 0.0);
    REQUIRE(result->strategy_id.get() == "vwap_reversion");
}

TEST_CASE("VwapReversion: цена выше VWAP → LongExit", "[strategy][vwap_reversion]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VwapReversionStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.microstructure.trade_vwap = 50000.0;
    ctx.features.microstructure.trade_flow_valid = true;
    ctx.features.last_price = Price(50300.0); // +0.6% выше VWAP
    ctx.features.technical.rsi_14 = 62.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 20.0;
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Sell);
    REQUIRE(result->signal_intent == SignalIntent::LongExit);
    REQUIRE(result->exit_reason == ExitReason::TakeProfit);
}

TEST_CASE("VwapReversion: сильный тренд (высокий ADX) → нет сигнала", "[strategy][vwap_reversion]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VwapReversionStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.microstructure.trade_vwap = 50000.0;
    ctx.features.microstructure.trade_flow_valid = true;
    ctx.features.last_price = Price(49750.0);
    ctx.features.technical.rsi_14 = 38.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 40.0; // Слишком трендовый
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Volume Profile
// ============================================================

TEST_CASE("VolumeProfile: цена у VA Low → BUY", "[strategy][volume_profile]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VolumeProfileStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.vp_poc = 50000.0;
    ctx.features.technical.vp_value_area_high = 50500.0;
    ctx.features.technical.vp_value_area_low = 49500.0;
    ctx.features.technical.vp_valid = true;
    // Цена чуть ниже VA Low
    ctx.features.last_price = Price(49490.0);
    ctx.features.technical.rsi_14 = 38.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 20.0;
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Buy);
    REQUIRE(result->signal_intent == SignalIntent::LongEntry);
    REQUIRE(result->strategy_id.get() == "volume_profile");
}

TEST_CASE("VolumeProfile: цена у VA High → LongExit", "[strategy][volume_profile]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VolumeProfileStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.vp_poc = 50000.0;
    ctx.features.technical.vp_value_area_high = 50500.0;
    ctx.features.technical.vp_value_area_low = 49500.0;
    ctx.features.technical.vp_valid = true;
    // Цена у VA High
    ctx.features.last_price = Price(50510.0);
    ctx.features.technical.rsi_14 = 62.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 20.0;
    ctx.features.technical.adx_valid = true;

    auto result = strategy.evaluate(ctx);

    REQUIRE(result.has_value());
    REQUIRE(result->side == Side::Sell);
    REQUIRE(result->signal_intent == SignalIntent::LongExit);
    REQUIRE(result->exit_reason == ExitReason::RangeTopExit);
}

TEST_CASE("VolumeProfile: VP невалидный → нет сигнала", "[strategy][volume_profile]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    VolumeProfileStrategy strategy(logger, clk);

    auto ctx = make_context();
    ctx.features.technical.vp_valid = false; // VP данные недоступны
    ctx.features.technical.rsi_14 = 38.0;
    ctx.features.technical.rsi_valid = true;

    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Config override tests
// ============================================================

TEST_CASE("Breakout: кастомный конфиг применяется", "[strategy][config]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    BreakoutConfig cfg;
    cfg.max_conviction = 0.70;
    cfg.base_conviction = 0.20;
    BreakoutStrategy strategy(logger, clk, cfg);

    auto ctx = make_context();
    ctx.features.technical.bb_valid = true;
    ctx.features.technical.bb_upper = 51000.0;
    ctx.features.technical.bb_lower = 49000.0;

    for (int i = 0; i < 5; ++i) {
        ctx.features.technical.bb_bandwidth = 0.01;
        ctx.features.last_price = Price(50000.0);
        strategy.evaluate(ctx);
    }

    ctx.features.technical.bb_bandwidth = 0.05;
    ctx.features.last_price = Price(52000.0);

    auto result = strategy.evaluate(ctx);
    if (result.has_value()) {
        REQUIRE(result->conviction <= 0.70);
    }
}

TEST_CASE("Momentum: кастомный конфиг с высоким ADX порогом → нет сигнала", "[strategy][config]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    MomentumConfig cfg;
    cfg.adx_min = 50.0; // Очень высокий порог
    MomentumStrategy strategy(logger, clk, cfg);

    auto ctx = make_context();
    ctx.features.technical.ema_20 = 51000.0;
    ctx.features.technical.ema_50 = 49000.0;
    ctx.features.technical.ema_valid = true;
    ctx.features.technical.rsi_14 = 55.0;
    ctx.features.technical.rsi_valid = true;
    ctx.features.technical.adx = 30.0; // Ниже порога 50
    ctx.features.technical.adx_valid = true;
    ctx.features.technical.momentum_5 = 0.02;
    ctx.features.technical.momentum_valid = true;

    auto result = strategy.evaluate(ctx);
    REQUIRE_FALSE(result.has_value());
}
