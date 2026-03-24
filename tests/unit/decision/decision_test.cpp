#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "decision/decision_aggregation_engine.hpp"
#include "strategy/strategy_types.hpp"
#include "strategy_allocator/allocation_types.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::decision;
using namespace tb::strategy;
using namespace tb::strategy_allocator;
using namespace tb::regime;
using namespace tb::world_model;
using namespace tb::uncertainty;

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

TradeIntent make_intent(const std::string& id, Side side, double conviction) {
    TradeIntent intent;
    intent.strategy_id = StrategyId(id);
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = Symbol("BTCUSDT");
    intent.side = side;
    intent.conviction = conviction;
    intent.signal_name = "test_signal";
    intent.generated_at = Timestamp(1000000);
    return intent;
}

AllocationResult make_allocation(const std::vector<std::pair<std::string, double>>& allocs) {
    AllocationResult result;
    for (const auto& [id, weight] : allocs) {
        StrategyAllocation a;
        a.strategy_id = StrategyId(id);
        a.is_enabled = weight > 0.0;
        a.weight = weight;
        a.size_multiplier = 1.0;
        result.allocations.push_back(std::move(a));
        if (weight > 0.0) ++result.enabled_count;
    }
    return result;
}

RegimeSnapshot make_regime() {
    RegimeSnapshot rs;
    rs.label = RegimeLabel::Trending;
    rs.detailed = DetailedRegime::StrongUptrend;
    rs.confidence = 0.8;
    return rs;
}

WorldModelSnapshot make_world() {
    WorldModelSnapshot ws;
    ws.state = WorldState::StableTrendContinuation;
    ws.label = WorldStateLabel::Stable;
    ws.fragility = {0.2, true};
    return ws;
}

UncertaintySnapshot make_low_uncertainty() {
    UncertaintySnapshot us;
    us.level = UncertaintyLevel::Low;
    us.aggregate_score = 0.15;
    us.recommended_action = UncertaintyAction::Normal;
    us.size_multiplier = 0.85;
    us.threshold_multiplier = 1.3;
    return us;
}

UncertaintySnapshot make_extreme_uncertainty() {
    UncertaintySnapshot us;
    us.level = UncertaintyLevel::Extreme;
    us.aggregate_score = 0.85;
    us.recommended_action = UncertaintyAction::NoTrade;
    us.size_multiplier = 0.15;
    us.threshold_multiplier = 2.7;
    return us;
}

} // anonymous namespace

TEST_CASE("Decision: один интент одобрен → trade_approved", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 0.7}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE(record.trade_approved);
    REQUIRE(record.final_intent.has_value());
    REQUIRE(record.final_intent->side == Side::Buy);
    REQUIRE(record.final_conviction > 0.0);
    REQUIRE_FALSE(record.rationale.empty());
}

TEST_CASE("Decision: высокая неопределённость → глобальное вето", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 1.0}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_extreme_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
    REQUIRE_FALSE(record.global_vetoes.empty());
    REQUIRE(record.global_vetoes[0].source == "uncertainty");
}

TEST_CASE("Decision: конфликт BUY и SELL → вето", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto buy_intent = make_intent("momentum", Side::Buy, 0.7);
    auto sell_intent = make_intent("mean_reversion", Side::Sell, 0.6);
    auto allocation = make_allocation({{"momentum", 0.5}, {"mean_reversion", 0.5}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {buy_intent, sell_intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
    // Должна быть причина вето — конфликт
    bool has_conflict = false;
    for (const auto& v : record.global_vetoes) {
        if (v.source == "conflict") has_conflict = true;
    }
    REQUIRE(has_conflict);
}

TEST_CASE("Decision: низкая conviction → не одобрен", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.1); // Очень низкая conviction
    auto allocation = make_allocation({{"momentum", 0.5}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
}

TEST_CASE("Decision: полный DecisionRecord содержит rationale", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 1.0}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE_FALSE(record.rationale.empty());
    REQUIRE(record.symbol.get() == "BTCUSDT");
    REQUIRE(record.is_reconstructable());
}

TEST_CASE("Decision: детерминизм — одинаковые входы → одинаковый результат", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 0.7}});
    auto regime = make_regime();
    auto world = make_world();
    auto uncertainty = make_low_uncertainty();

    auto record1 = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation, regime, world, uncertainty);
    auto record2 = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation, regime, world, uncertainty);

    REQUIRE(record1.trade_approved == record2.trade_approved);
    REQUIRE(record1.final_conviction == record2.final_conviction);
}
