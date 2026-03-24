/**
 * @file scenario_simulator.hpp
 * @brief Симулятор синтетических сценариев
 *
 * Генерирует и прогоняет стресс-сценарии для проверки
 * устойчивости системы к враждебным рыночным условиям.
 */
#pragma once

#include "scenario_types.hpp"
#include "adversarial_defense/adversarial_defense.hpp"

#include <vector>

namespace tb::scenarios {

/// Симулятор синтетических сценариев
class SyntheticScenarioSimulator {
public:
    SyntheticScenarioSimulator();

    /// Сгенерировать шаги сценария
    std::vector<ScenarioStep> generate_scenario(const ScenarioConfig& config) const;

    /// Получить ожидаемую реакцию для шага
    ExpectedReaction get_expected_reaction(ScenarioCategory category,
                                           const ScenarioStep& step) const;

    /// Запустить сценарий с AdversarialMarketDefense и получить результат
    ScenarioResult run_scenario(const ScenarioConfig& config,
                                adversarial::AdversarialMarketDefense& defense);

    /// Получить все предустановленные сценарии
    static std::vector<ScenarioConfig> get_preset_scenarios();

private:
    /// Генераторы для каждой категории
    std::vector<ScenarioStep> generate_spread_explosion(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_stale_feed_burst(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_reconnect_storm(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_order_book_desync(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_exchange_slowdown(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_reject_storm(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_liquidity_collapse(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_ai_timeout_storm(const ScenarioConfig& cfg) const;
    std::vector<ScenarioStep> generate_execution_toxicity(const ScenarioConfig& cfg) const;
};

} // namespace tb::scenarios
