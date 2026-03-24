/**
 * @file test_scenario_integration.cpp
 * @brief Интеграционные тесты: синтетические сценарии + защита от угроз
 */

#include <catch2/catch_all.hpp>

#include "synthetic_scenarios/scenario_simulator.hpp"
#include "synthetic_scenarios/scenario_types.hpp"
#include "adversarial_defense/adversarial_defense.hpp"

using namespace tb;
using namespace tb::scenarios;
using namespace tb::adversarial;

TEST_CASE("Интеграция — spread explosion обнаруживается защитой", "[scenario_integration]") {
    SyntheticScenarioSimulator simulator;
    AdversarialMarketDefense defense;

    ScenarioConfig config;
    config.category = ScenarioCategory::SpreadExplosion;
    config.name = "spread_explosion_integration";
    config.duration_steps = 10;
    config.intensity = 0.8;

    auto result = simulator.run_scenario(config, defense);

    CHECK(result.category == ScenarioCategory::SpreadExplosion);
    CHECK(result.total_steps == 10);
    // Защита должна сохранить безопасность
    CHECK(result.safety_maintained);
    // При высокой интенсивности деградация активируется
    CHECK(result.degraded_mode_triggered);
}

TEST_CASE("Интеграция — liquidity collapse обнаруживается защитой", "[scenario_integration]") {
    SyntheticScenarioSimulator simulator;
    AdversarialMarketDefense defense;

    ScenarioConfig config;
    config.category = ScenarioCategory::LiquidityCollapse;
    config.name = "liquidity_collapse_integration";
    config.duration_steps = 10;
    config.intensity = 0.9;

    auto result = simulator.run_scenario(config, defense);

    CHECK(result.category == ScenarioCategory::LiquidityCollapse);
    CHECK(result.total_steps == 10);
    CHECK(result.safety_maintained);
}

TEST_CASE("Интеграция — все 9 сценариев генерируются и прогоняются", "[scenario_integration]") {
    SyntheticScenarioSimulator simulator;
    AdversarialMarketDefense defense;

    auto presets = SyntheticScenarioSimulator::get_preset_scenarios();
    REQUIRE(presets.size() == 9);

    for (const auto& preset : presets) {
        INFO("Сценарий: " << preset.name);

        // Генерация шагов не должна падать
        auto steps = simulator.generate_scenario(preset);
        CHECK_FALSE(steps.empty());

        // Прогон с защитой
        auto result = simulator.run_scenario(preset, defense);
        CHECK(result.total_steps > 0);
        CHECK(result.safety_maintained);
    }
}

TEST_CASE("Интеграция — reject storm прогоняется безопасно", "[scenario_integration]") {
    SyntheticScenarioSimulator simulator;
    AdversarialMarketDefense defense;

    ScenarioConfig config;
    config.category = ScenarioCategory::RejectStorm;
    config.name = "reject_storm_integration";
    config.duration_steps = 10;
    config.intensity = 0.7;

    auto result = simulator.run_scenario(config, defense);

    CHECK(result.category == ScenarioCategory::RejectStorm);
    CHECK(result.safety_maintained);
    CHECK(result.total_steps == 10);
}
