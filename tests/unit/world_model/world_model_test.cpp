#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "world_model/world_model_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::world_model;
using namespace tb::features;

// ============================================================
// Вспомогательные классы для тестов
// ============================================================

namespace {

/// Простой логгер-заглушка для тестов
class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

/// Часы с фиксированным временем для детерминизма
class TestClock : public clock::IClock {
public:
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
};

/// Создаёт базовый снимок с заданными параметрами
FeatureSnapshot make_snapshot(const std::string& sym = "BTCUSDT") {
    FeatureSnapshot snap;
    snap.symbol = Symbol(sym);
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    return snap;
}

/// Конфигурация для StableTrendContinuation
FeatureSnapshot make_trend_snapshot() {
    auto snap = make_snapshot();
    snap.technical.adx = 30.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 55.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.technical.ema_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;
    return snap;
}

} // anonymous namespace

// ============================================================
// Тесты
// ============================================================

TEST_CASE("WorldModel: StableTrendContinuation — высокий ADX, умеренный RSI", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::StableTrendContinuation);
    REQUIRE(result.label == WorldStateLabel::Stable);
    REQUIRE(result.fragility.valid);
    REQUIRE(result.fragility.value < 0.5); // Низкая хрупкость
}

TEST_CASE("WorldModel: ChopNoise — низкий ADX, средний RSI", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_snapshot();
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::ChopNoise);
    REQUIRE(result.label == WorldStateLabel::Stable);
}

TEST_CASE("WorldModel: ExhaustionSpike — экстремальный RSI", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_snapshot();
    snap.technical.rsi_14 = 85.0;
    snap.technical.rsi_valid = true;
    snap.technical.momentum_5 = 0.05;
    snap.technical.momentum_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::ExhaustionSpike);
    REQUIRE(result.label == WorldStateLabel::Disrupted);
    REQUIRE(result.fragility.value > 0.7); // Высокая хрупкость
}

TEST_CASE("WorldModel: LiquidityVacuum — очень широкий спред", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_snapshot();
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 60.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::LiquidityVacuum);
    REQUIRE(result.label == WorldStateLabel::Disrupted);
    REQUIRE(result.fragility.value > 0.8);
}

TEST_CASE("WorldModel: Unknown — нет данных", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_snapshot();
    // Все индикаторы невалидны по умолчанию

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::Unknown);
    REQUIRE(result.label == WorldStateLabel::Unknown);
}

TEST_CASE("WorldModel: to_label корректность маппинга", "[world_model]") {
    REQUIRE(WorldModelSnapshot::to_label(WorldState::StableTrendContinuation) == WorldStateLabel::Stable);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::FragileBreakout) == WorldStateLabel::Transitioning);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ExhaustionSpike) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::LiquidityVacuum) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ToxicMicrostructure) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::Unknown) == WorldStateLabel::Unknown);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ChopNoise) == WorldStateLabel::Stable);
}

TEST_CASE("WorldModel: current_state возвращает последний снимок", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    // До обновления — нет данных
    REQUIRE_FALSE(engine.current_state(Symbol("BTCUSDT")).has_value());

    auto snap = make_trend_snapshot();
    engine.update(snap);

    auto state = engine.current_state(Symbol("BTCUSDT"));
    REQUIRE(state.has_value());
    REQUIRE(state->state == WorldState::StableTrendContinuation);
}

TEST_CASE("WorldModel: strategy_suitability заполняется", "[world_model]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedWorldModelEngine engine(logger, clk);

    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE_FALSE(result.strategy_suitability.empty());
    // Для StableTrendContinuation momentum должен иметь высокую пригодность
    bool found_momentum = false;
    for (const auto& s : result.strategy_suitability) {
        if (s.strategy_id.get() == "momentum") {
            found_momentum = true;
            REQUIRE(s.suitability > 0.5);
        }
    }
    REQUIRE(found_momentum);
}
