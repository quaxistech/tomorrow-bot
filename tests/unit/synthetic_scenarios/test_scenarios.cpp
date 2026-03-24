/**
 * @file test_scenarios.cpp
 * @brief Тесты модуля синтетических сценариев
 */

#include <catch2/catch_all.hpp>

#include "synthetic_scenarios/scenario_simulator.hpp"
#include "synthetic_scenarios/scenario_types.hpp"
#include "adversarial_defense/adversarial_defense.hpp"

using namespace tb;
using namespace tb::scenarios;
using namespace tb::adversarial;

TEST_CASE("SyntheticScenarioSimulator — предустановленные сценарии", "[scenarios]") {
    auto presets = SyntheticScenarioSimulator::get_preset_scenarios();

    CHECK(presets.size() == 9);

    // Проверяем что все категории представлены
    bool found_spread = false, found_stale = false, found_reconnect = false;
    bool found_desync = false, found_slowdown = false, found_reject = false;
    bool found_liquidity = false, found_ai = false, found_toxicity = false;

    for (const auto& p : presets) {
        switch (p.category) {
            case ScenarioCategory::SpreadExplosion:   found_spread = true; break;
            case ScenarioCategory::StaleFeedBurst:    found_stale = true; break;
            case ScenarioCategory::ReconnectStorm:    found_reconnect = true; break;
            case ScenarioCategory::OrderBookDesync:   found_desync = true; break;
            case ScenarioCategory::ExchangeSlowdown:  found_slowdown = true; break;
            case ScenarioCategory::RejectStorm:       found_reject = true; break;
            case ScenarioCategory::LiquidityCollapse: found_liquidity = true; break;
            case ScenarioCategory::AITimeoutStorm:    found_ai = true; break;
            case ScenarioCategory::ExecutionToxicity: found_toxicity = true; break;
        }
    }

    CHECK(found_spread);
    CHECK(found_stale);
    CHECK(found_reconnect);
    CHECK(found_desync);
    CHECK(found_slowdown);
    CHECK(found_reject);
    CHECK(found_liquidity);
    CHECK(found_ai);
    CHECK(found_toxicity);
}

TEST_CASE("SyntheticScenarioSimulator — генерация spread explosion", "[scenarios]") {
    SyntheticScenarioSimulator simulator;

    ScenarioConfig config;
    config.category = ScenarioCategory::SpreadExplosion;
    config.name = "test_spread";
    config.duration_steps = 10;
    config.intensity = 0.8;

    auto steps = simulator.generate_scenario(config);

    REQUIRE(steps.size() == 10);

    // Спред должен расти от шага к шагу
    for (size_t i = 1; i < steps.size(); ++i) {
        CHECK(steps[i].market_condition.spread_bps >=
              steps[i - 1].market_condition.spread_bps);
    }

    // Первый шаг — нормальный спред, последний — экстремальный
    CHECK(steps.front().market_condition.spread_bps < 50.0);
    CHECK(steps.back().market_condition.spread_bps > 100.0);
}

TEST_CASE("SyntheticScenarioSimulator — генерация liquidity collapse", "[scenarios]") {
    SyntheticScenarioSimulator simulator;

    ScenarioConfig config;
    config.category = ScenarioCategory::LiquidityCollapse;
    config.name = "test_liquidity";
    config.duration_steps = 10;
    config.intensity = 0.9;

    auto steps = simulator.generate_scenario(config);

    REQUIRE(steps.size() == 10);

    // Глубина должна уменьшаться
    for (size_t i = 1; i < steps.size(); ++i) {
        CHECK(steps[i].market_condition.bid_depth <=
              steps[i - 1].market_condition.bid_depth + 0.01);
    }

    // Последний шаг — очень низкая глубина
    CHECK(steps.back().market_condition.bid_depth < 50.0);
}

TEST_CASE("SyntheticScenarioSimulator — прогон сценария", "[scenarios]") {
    SyntheticScenarioSimulator simulator;
    AdversarialMarketDefense defense;

    ScenarioConfig config;
    config.category = ScenarioCategory::SpreadExplosion;
    config.name = "test_run";
    config.duration_steps = 10;
    config.intensity = 0.8;

    auto result = simulator.run_scenario(config, defense);

    CHECK(result.category == ScenarioCategory::SpreadExplosion);
    CHECK(result.name == "test_run");
    CHECK(result.total_steps == 10);
    CHECK(result.steps_with_correct_reaction > 0);
    // При высокой интенсивности защита должна активироваться
    CHECK(result.degraded_mode_triggered);
}

TEST_CASE("SyntheticScenarioSimulator — ожидаемые реакции", "[scenarios]") {
    SyntheticScenarioSimulator simulator;

    SECTION("SpreadExplosion — высокий спред") {
        ScenarioStep step;
        step.market_condition.spread_bps = 200.0;

        auto reaction = simulator.get_expected_reaction(
            ScenarioCategory::SpreadExplosion, step);

        CHECK(reaction.should_veto_trade);
        CHECK(reaction.should_alert_operator);
    }

    SECTION("LiquidityCollapse — низкая глубина") {
        ScenarioStep step;
        step.market_condition.bid_depth = 5.0;
        step.market_condition.ask_depth = 100.0;

        auto reaction = simulator.get_expected_reaction(
            ScenarioCategory::LiquidityCollapse, step);

        CHECK(reaction.should_veto_trade);
    }

    SECTION("OrderBookDesync — невалидный стакан") {
        ScenarioStep step;
        step.market_condition.book_valid = false;

        auto reaction = simulator.get_expected_reaction(
            ScenarioCategory::OrderBookDesync, step);

        CHECK(reaction.should_veto_trade);
    }
}

TEST_CASE("SyntheticScenarioSimulator — to_string функции", "[scenarios]") {
    CHECK(to_string(ScenarioCategory::SpreadExplosion) == "SpreadExplosion");
    CHECK(to_string(ScenarioCategory::LiquidityCollapse) == "LiquidityCollapse");
    CHECK(to_string(ScenarioCategory::ExecutionToxicity) == "ExecutionToxicity");
}
