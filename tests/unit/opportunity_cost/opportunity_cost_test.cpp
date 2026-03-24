#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "opportunity_cost/opportunity_cost_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::opportunity_cost;
using namespace Catch::Matchers;

// ========== Тестовые заглушки ==========

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

// ========== Вспомогательные функции ==========

static strategy::TradeIntent make_intent(double conviction = 0.7) {
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("momentum_v1");
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.suggested_quantity = Quantity(0.1);
    intent.conviction = conviction;
    intent.urgency = 0.5;
    intent.correlation_id = CorrelationId("test-corr-1");
    return intent;
}

static execution_alpha::ExecutionAlphaResult make_exec_alpha(double total_cost_bps = 5.0) {
    execution_alpha::ExecutionAlphaResult result;
    result.recommended_style = execution_alpha::ExecutionStyle::Passive;
    result.quality.total_cost_bps = total_cost_bps;
    result.quality.spread_cost_bps = 3.0;
    result.quality.estimated_slippage_bps = 2.0;
    result.should_execute = true;
    return result;
}

static RuleBasedOpportunityCost make_engine() {
    return RuleBasedOpportunityCost(
        RuleBasedOpportunityCost::Config{},
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>());
}

// ========== Тесты ==========

TEST_CASE("OpportunityCost: Высокая conviction, низкая стоимость → Execute", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.8);        // Высокая conviction
    auto exec_alpha = make_exec_alpha(5.0); // Низкая стоимость

    auto result = engine.evaluate(intent, exec_alpha, 0.2, 0.3);

    REQUIRE(result.action == OpportunityAction::Execute);
    REQUIRE(result.score.net_expected_bps > 0.0);
}

TEST_CASE("OpportunityCost: Низкая conviction ниже порога → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.1);         // Очень низкая conviction
    auto exec_alpha = make_exec_alpha(5.0);

    auto result = engine.evaluate(intent, exec_alpha, 0.2, 0.3);

    REQUIRE(result.action == OpportunityAction::Suppress);
}

TEST_CASE("OpportunityCost: Высокая экспозиция при средней conviction → Defer", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5);           // Средняя conviction
    auto exec_alpha = make_exec_alpha(5.0);

    // Высокая экспозиция (85%), conviction < 0.7 (порог high_exposure_min_conviction)
    auto result = engine.evaluate(intent, exec_alpha, 0.85, 0.3);

    REQUIRE(result.action == OpportunityAction::Defer);
}

TEST_CASE("OpportunityCost: Отрицательный чистый доход → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.01);           // Очень низкая conviction → ~1 бп дохода
    auto exec_alpha = make_exec_alpha(50.0);   // Высокая стоимость → 50 бп

    auto result = engine.evaluate(intent, exec_alpha, 0.2, 0.0);

    // net_expected = 1 - 50 = -49 бп → Suppress
    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.score.net_expected_bps < 0.0);
}

TEST_CASE("OpportunityCost: to_string работает корректно", "[opportunity_cost]") {
    REQUIRE(to_string(OpportunityAction::Execute) == "Execute");
    REQUIRE(to_string(OpportunityAction::Defer) == "Defer");
    REQUIRE(to_string(OpportunityAction::Suppress) == "Suppress");
    REQUIRE(to_string(OpportunityAction::Upgrade) == "Upgrade");
}
