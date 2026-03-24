/**
 * @file scenario_simulator.cpp
 * @brief Реализация симулятора синтетических сценариев
 */

#include "scenario_simulator.hpp"

#include <algorithm>
#include <cmath>

namespace tb::scenarios {

SyntheticScenarioSimulator::SyntheticScenarioSimulator() = default;

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_scenario(
    const ScenarioConfig& config) const {

    switch (config.category) {
        case ScenarioCategory::SpreadExplosion:   return generate_spread_explosion(config);
        case ScenarioCategory::StaleFeedBurst:    return generate_stale_feed_burst(config);
        case ScenarioCategory::ReconnectStorm:    return generate_reconnect_storm(config);
        case ScenarioCategory::OrderBookDesync:   return generate_order_book_desync(config);
        case ScenarioCategory::ExchangeSlowdown:  return generate_exchange_slowdown(config);
        case ScenarioCategory::RejectStorm:       return generate_reject_storm(config);
        case ScenarioCategory::LiquidityCollapse: return generate_liquidity_collapse(config);
        case ScenarioCategory::AITimeoutStorm:    return generate_ai_timeout_storm(config);
        case ScenarioCategory::ExecutionToxicity: return generate_execution_toxicity(config);
    }
    return {};
}

ExpectedReaction SyntheticScenarioSimulator::get_expected_reaction(
    ScenarioCategory category, const ScenarioStep& step) const {

    ExpectedReaction reaction;

    switch (category) {
        case ScenarioCategory::SpreadExplosion:
            // При высоком спреде — вето на торговлю
            if (step.market_condition.spread_bps > 100.0) {
                reaction.should_veto_trade = true;
                reaction.should_alert_operator = true;
                reaction.description = "Спред экстремальный — торговля запрещена";
            }
            break;

        case ScenarioCategory::StaleFeedBurst:
            if (step.feed_stale) {
                reaction.should_enter_degraded = true;
                reaction.description = "Устаревшие данные — деградированный режим";
            }
            break;

        case ScenarioCategory::ReconnectStorm:
            if (step.connection_lost) {
                reaction.should_veto_trade = true;
                reaction.should_enter_degraded = true;
                reaction.description = "Потеря соединения — запрет торговли";
            }
            break;

        case ScenarioCategory::OrderBookDesync:
            if (!step.market_condition.book_valid) {
                reaction.should_veto_trade = true;
                reaction.should_alert_operator = true;
                reaction.description = "Стакан невалиден — вето";
            }
            break;

        case ScenarioCategory::ExchangeSlowdown:
            if (step.simulated_latency_ms > 5000) {
                reaction.should_enter_degraded = true;
                reaction.should_alert_operator = true;
                reaction.description = "Задержка биржи критична";
            }
            break;

        case ScenarioCategory::RejectStorm:
            if (step.order_rejected) {
                reaction.should_trigger_cooldown = true;
                reaction.description = "Шторм отклонений — cooldown";
            }
            break;

        case ScenarioCategory::LiquidityCollapse:
            if (step.market_condition.bid_depth < 10.0 || step.market_condition.ask_depth < 10.0) {
                reaction.should_veto_trade = true;
                reaction.description = "Ликвидность исчерпана — вето";
            }
            break;

        case ScenarioCategory::AITimeoutStorm:
            if (step.simulated_latency_ms > 10000) {
                reaction.should_activate_kill_switch = true;
                reaction.description = "Таймаут AI — kill switch";
            }
            break;

        case ScenarioCategory::ExecutionToxicity:
            if (step.market_condition.spread_bps > 50.0 &&
                step.market_condition.book_imbalance > 0.7) {
                reaction.should_veto_trade = true;
                reaction.description = "Токсичность исполнения — вето";
            }
            break;
    }

    return reaction;
}

ScenarioResult SyntheticScenarioSimulator::run_scenario(
    const ScenarioConfig& config, adversarial::AdversarialMarketDefense& defense) {

    ScenarioResult result;
    result.category = config.category;
    result.name = config.name;
    result.started_at = Timestamp(0);

    auto steps = generate_scenario(config);
    result.total_steps = static_cast<int>(steps.size());

    for (const auto& step : steps) {
        auto assessment = defense.assess(step.market_condition);
        auto expected = get_expected_reaction(config.category, step);

        bool correct = true;

        // Проверяем: если ожидается veto — должно быть unsafe
        if (expected.should_veto_trade && assessment.is_safe) {
            correct = false;
            result.issues.push_back(
                "Шаг " + std::to_string(step.step_number) +
                ": ожидалось вето, но система считает безопасным");
            result.safety_maintained = false;
        }

        // Проверяем: если ожидается degraded — должен быть снижен множитель уверенности
        if (expected.should_enter_degraded && assessment.confidence_multiplier > 0.8) {
            result.observations.push_back(
                "Шаг " + std::to_string(step.step_number) +
                ": ожидалось снижение уверенности");
        }

        if (!assessment.is_safe) {
            result.degraded_mode_triggered = true;
        }

        if (correct) {
            result.steps_with_correct_reaction++;
        }
    }

    result.completed_at = Timestamp(static_cast<int64_t>(result.total_steps) * 1'000'000);
    return result;
}

std::vector<ScenarioConfig> SyntheticScenarioSimulator::get_preset_scenarios() {
    return {
        {ScenarioCategory::SpreadExplosion,   "spread_explosion",   "Взрыв спреда до экстремальных значений", 10, 0.8, {}},
        {ScenarioCategory::StaleFeedBurst,    "stale_feed_burst",   "Серия устаревших данных фида",           10, 0.6, {}},
        {ScenarioCategory::ReconnectStorm,    "reconnect_storm",    "Шторм переподключений к бирже",          10, 0.7, {}},
        {ScenarioCategory::OrderBookDesync,   "order_book_desync",  "Рассинхронизация стакана",               10, 0.9, {}},
        {ScenarioCategory::ExchangeSlowdown,  "exchange_slowdown",  "Замедление биржи",                       10, 0.5, {}},
        {ScenarioCategory::RejectStorm,       "reject_storm",       "Шторм отклонений ордеров",               10, 0.8, {}},
        {ScenarioCategory::LiquidityCollapse, "liquidity_collapse", "Обвал ликвидности",                      10, 0.9, {}},
        {ScenarioCategory::AITimeoutStorm,    "ai_timeout_storm",   "Шторм таймаутов AI",                     10, 0.7, {}},
        {ScenarioCategory::ExecutionToxicity, "execution_toxicity", "Токсичность исполнения",                 10, 0.8, {}},
    };
}

// --- Генераторы сценариев ---

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_spread_explosion(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    steps.reserve(cfg.duration_steps);

    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        // Спред растёт от нормального (5 bps) до экстремального (500 * intensity)
        double spread = 5.0 + progress * 500.0 * cfg.intensity;

        ScenarioStep step;
        step.step_number = i;
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = spread,
            .book_imbalance = 0.3,
            .bid_depth = 100.0,
            .ask_depth = 100.0,
            .book_instability = progress * 0.5,
            .buy_sell_ratio = 0.5,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = "Спред: " + std::to_string(spread) + " bps";
        steps.push_back(std::move(step));
    }

    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_stale_feed_burst(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        ScenarioStep step;
        step.step_number = i;
        step.feed_stale = (i >= 2); // Данные устаревают начиная с 3-го шага
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 10.0,
            .book_imbalance = 0.2,
            .bid_depth = 80.0,
            .ask_depth = 80.0,
            .book_instability = 0.1,
            .buy_sell_ratio = 0.5,
            .book_valid = !step.feed_stale,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = step.feed_stale ? "Данные устарели" : "Данные актуальны";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_reconnect_storm(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        ScenarioStep step;
        step.step_number = i;
        step.connection_lost = (i % 2 == 1); // Каждый нечётный шаг — потеря соединения
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = step.connection_lost ? 50.0 : 10.0,
            .book_imbalance = 0.3,
            .bid_depth = 60.0,
            .ask_depth = 60.0,
            .book_instability = step.connection_lost ? 0.6 : 0.1,
            .buy_sell_ratio = 0.5,
            .book_valid = !step.connection_lost,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = step.connection_lost ? "Соединение потеряно" : "Соединение восстановлено";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_order_book_desync(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        ScenarioStep step;
        step.step_number = i;
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 15.0 + progress * 100.0 * cfg.intensity,
            .book_imbalance = progress * 0.9,
            .bid_depth = 100.0 * (1.0 - progress * 0.8),
            .ask_depth = 100.0 * (1.0 - progress * 0.7),
            .book_instability = progress * cfg.intensity,
            .buy_sell_ratio = 0.5,
            .book_valid = (i < cfg.duration_steps / 2),
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = step.market_condition.book_valid
            ? "Стакан валиден, нестабильность растёт"
            : "Стакан рассинхронизирован";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_exchange_slowdown(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        ScenarioStep step;
        step.step_number = i;
        step.exchange_slow = (progress > 0.3);
        step.simulated_latency_ms = static_cast<int64_t>(progress * 15000.0 * cfg.intensity);
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 10.0 + progress * 30.0,
            .book_imbalance = 0.2,
            .bid_depth = 80.0,
            .ask_depth = 80.0,
            .book_instability = 0.2,
            .buy_sell_ratio = 0.5,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = "Задержка: " + std::to_string(step.simulated_latency_ms) + " мс";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_reject_storm(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        ScenarioStep step;
        step.step_number = i;
        step.order_rejected = (i >= 1); // Отклонения начинаются со 2-го шага
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 15.0,
            .book_imbalance = 0.3,
            .bid_depth = 70.0,
            .ask_depth = 70.0,
            .book_instability = 0.3,
            .buy_sell_ratio = 0.5,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = step.order_rejected ? "Ордер отклонён" : "Ордер принят";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_liquidity_collapse(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        // Глубина падает от нормальной (200) до почти нуля
        double depth = 200.0 * (1.0 - progress * cfg.intensity);

        ScenarioStep step;
        step.step_number = i;
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 10.0 + progress * 200.0 * cfg.intensity,
            .book_imbalance = progress * 0.6,
            .bid_depth = depth,
            .ask_depth = depth * 0.8,
            .book_instability = progress * 0.5,
            .buy_sell_ratio = 0.5,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = "Глубина: " + std::to_string(depth);
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_ai_timeout_storm(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        ScenarioStep step;
        step.step_number = i;
        step.simulated_latency_ms = static_cast<int64_t>(progress * 20000.0 * cfg.intensity);
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 10.0,
            .book_imbalance = 0.2,
            .bid_depth = 100.0,
            .ask_depth = 100.0,
            .book_instability = 0.1,
            .buy_sell_ratio = 0.5,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = "Задержка AI: " + std::to_string(step.simulated_latency_ms) + " мс";
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<ScenarioStep> SyntheticScenarioSimulator::generate_execution_toxicity(
    const ScenarioConfig& cfg) const {

    std::vector<ScenarioStep> steps;
    for (int i = 0; i < cfg.duration_steps; ++i) {
        double progress = static_cast<double>(i) / std::max(1, cfg.duration_steps - 1);
        ScenarioStep step;
        step.step_number = i;
        step.market_condition = adversarial::MarketCondition{
            .symbol = Symbol("TEST"),
            .spread_bps = 5.0 + progress * 300.0 * cfg.intensity,
            .book_imbalance = 0.3 + progress * 0.6,
            .bid_depth = 100.0 * (1.0 - progress * 0.7),
            .ask_depth = 100.0 * (1.0 - progress * 0.5),
            .book_instability = progress * 0.6,
            .buy_sell_ratio = 0.5 + progress * 0.45 * cfg.intensity,
            .book_valid = true,
            .timestamp = Timestamp(static_cast<int64_t>(i) * 1'000'000)
        };
        step.description = "Токсичность растёт: спред " +
            std::to_string(step.market_condition.spread_bps) + " bps";
        steps.push_back(std::move(step));
    }
    return steps;
}

} // namespace tb::scenarios
