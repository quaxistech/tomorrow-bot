#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "uncertainty/uncertainty_engine.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::uncertainty;
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

FeatureSnapshot make_good_snapshot() {
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    snap.execution_context.is_feed_fresh = true;
    snap.technical.sma_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    snap.technical.macd_histogram = 0.5;
    snap.technical.macd_valid = true;
    snap.technical.adx = 30.0;
    snap.technical.adx_valid = true;
    snap.technical.bb_valid = true;
    snap.technical.ema_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 3.0;
    return snap;
}

regime::RegimeSnapshot make_confident_regime() {
    regime::RegimeSnapshot rs;
    rs.confidence = 0.85;
    rs.stability = 0.9;
    rs.label = RegimeLabel::Trending;
    return rs;
}

regime::RegimeSnapshot make_uncertain_regime() {
    regime::RegimeSnapshot rs;
    rs.confidence = 0.2;
    rs.stability = 0.3;
    rs.label = RegimeLabel::Unclear;
    return rs;
}

world_model::WorldModelSnapshot make_stable_world() {
    world_model::WorldModelSnapshot ws;
    ws.state = world_model::WorldState::StableTrendContinuation;
    ws.fragility = {0.2, true};
    return ws;
}

world_model::WorldModelSnapshot make_fragile_world() {
    world_model::WorldModelSnapshot ws;
    ws.state = world_model::WorldState::LiquidityVacuum;
    ws.fragility = {0.9, true};
    return ws;
}

} // anonymous namespace

TEST_CASE("Uncertainty: Низкая неопределённость — хорошие данные, уверенный режим", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap = make_good_snapshot();
    auto regime = make_confident_regime();
    auto world = make_stable_world();

    auto result = engine.assess(snap, regime, world);

    REQUIRE(result.level == UncertaintyLevel::Low);
    REQUIRE(result.recommended_action == UncertaintyAction::Normal);
    REQUIRE(result.aggregate_score < 0.3);
}

TEST_CASE("Uncertainty: Высокая неопределённость — плохие данные, неуверенный режим", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.book_quality = order_book::BookQuality::Stale;
    snap.execution_context.is_feed_fresh = false;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 40.0;
    // Мало валидных индикаторов

    auto regime = make_uncertain_regime();
    auto world = make_fragile_world();

    auto result = engine.assess(snap, regime, world);

    REQUIRE((result.level == UncertaintyLevel::High || result.level == UncertaintyLevel::Extreme));
    REQUIRE((result.recommended_action == UncertaintyAction::HigherThreshold ||
             result.recommended_action == UncertaintyAction::NoTrade));
}

TEST_CASE("Uncertainty: size_multiplier уменьшается с ростом неопределённости", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    // Низкая неопределённость
    auto snap_good = make_good_snapshot();
    auto result_low = engine.assess(snap_good, make_confident_regime(), make_stable_world());

    // Высокая неопределённость
    FeatureSnapshot snap_bad;
    snap_bad.symbol = Symbol("ETHUSDT");
    snap_bad.book_quality = order_book::BookQuality::Desynced;
    snap_bad.execution_context.is_feed_fresh = false;
    snap_bad.microstructure.spread_valid = true;
    snap_bad.microstructure.spread_bps = 50.0;

    auto result_high = engine.assess(snap_bad, make_uncertain_regime(), make_fragile_world());

    REQUIRE(result_low.size_multiplier > result_high.size_multiplier);
}

TEST_CASE("Uncertainty: threshold_multiplier растёт с неопределённостью", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap_good = make_good_snapshot();
    auto result_low = engine.assess(snap_good, make_confident_regime(), make_stable_world());

    FeatureSnapshot snap_bad;
    snap_bad.symbol = Symbol("ETHUSDT");
    snap_bad.book_quality = order_book::BookQuality::Stale;
    snap_bad.microstructure.spread_valid = true;
    snap_bad.microstructure.spread_bps = 40.0;
    snap_bad.execution_context.is_feed_fresh = false;

    auto result_high = engine.assess(snap_bad, make_uncertain_regime(), make_fragile_world());

    REQUIRE(result_low.threshold_multiplier < result_high.threshold_multiplier);
}

TEST_CASE("Uncertainty: Экстремальная неопределённость → NoTrade", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    // Максимально плохие условия
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.book_quality = order_book::BookQuality::Desynced;
    snap.execution_context.is_feed_fresh = false;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 80.0;
    snap.microstructure.liquidity_valid = true;
    snap.microstructure.liquidity_ratio = 5.0;
    snap.execution_context.slippage_valid = true;
    snap.execution_context.estimated_slippage_bps = 30.0;

    regime::RegimeSnapshot regime;
    regime.confidence = 0.1; // Почти нулевая уверенность

    world_model::WorldModelSnapshot world;
    world.fragility = {0.95, true};

    auto result = engine.assess(snap, regime, world);

    REQUIRE(result.level == UncertaintyLevel::Extreme);
    REQUIRE(result.recommended_action == UncertaintyAction::NoTrade);
    REQUIRE(result.size_multiplier < 0.3);
}
