/**
 * @file scenario_types.hpp
 * @brief Типы для синтетических сценариев тестирования
 *
 * Определяет категории сценариев, конфигурации, шаги и ожидаемые реакции.
 */
#pragma once

#include "common/types.hpp"
#include "adversarial_defense/adversarial_types.hpp"

#include <map>
#include <string>
#include <vector>

namespace tb::scenarios {

/// Категория сценария
enum class ScenarioCategory {
    SpreadExplosion,        ///< Взрыв спреда
    StaleFeedBurst,         ///< Серия устаревших данных
    ReconnectStorm,         ///< Шторм переподключений
    OrderBookDesync,        ///< Рассинхронизация стакана
    ExchangeSlowdown,       ///< Замедление биржи
    RejectStorm,            ///< Шторм отклонений ордеров
    LiquidityCollapse,      ///< Обвал ликвидности
    AITimeoutStorm,         ///< Шторм таймаутов AI
    ExecutionToxicity       ///< Токсичность исполнения
};

/// Преобразование категории в строку
inline std::string to_string(ScenarioCategory c) {
    switch (c) {
        case ScenarioCategory::SpreadExplosion:   return "SpreadExplosion";
        case ScenarioCategory::StaleFeedBurst:    return "StaleFeedBurst";
        case ScenarioCategory::ReconnectStorm:    return "ReconnectStorm";
        case ScenarioCategory::OrderBookDesync:   return "OrderBookDesync";
        case ScenarioCategory::ExchangeSlowdown:  return "ExchangeSlowdown";
        case ScenarioCategory::RejectStorm:       return "RejectStorm";
        case ScenarioCategory::LiquidityCollapse: return "LiquidityCollapse";
        case ScenarioCategory::AITimeoutStorm:    return "AITimeoutStorm";
        case ScenarioCategory::ExecutionToxicity: return "ExecutionToxicity";
    }
    return "Unknown";
}

/// Конфигурация сценария
struct ScenarioConfig {
    ScenarioCategory category;
    std::string name;
    std::string description;
    int duration_steps{10};            ///< Длительность сценария (шаги)
    double intensity{0.5};             ///< Интенсивность [0=мягкий, 1=экстремальный]
    std::map<std::string, double> parameters;  ///< Параметры сценария
};

/// Шаг сценария — генерируемые условия
struct ScenarioStep {
    int step_number{0};
    adversarial::MarketCondition market_condition;
    bool feed_stale{false};            ///< Данные устарели
    bool connection_lost{false};       ///< Соединение потеряно
    bool exchange_slow{false};         ///< Биржа замедлена
    bool order_rejected{false};        ///< Ордер отклонён
    int64_t simulated_latency_ms{0};   ///< Симулированная задержка (мс)
    std::string description;
};

/// Ожидаемая реакция системы на шаг
struct ExpectedReaction {
    bool should_veto_trade{false};              ///< Должна запретить сделку
    bool should_enter_degraded{false};          ///< Должна войти в деградированный режим
    bool should_trigger_cooldown{false};        ///< Должна активировать cooldown
    bool should_alert_operator{false};          ///< Должна уведомить оператора
    bool should_activate_kill_switch{false};    ///< Должна активировать kill switch
    std::string description;
};

/// Результат прогона сценария
struct ScenarioResult {
    ScenarioCategory category;
    std::string name;
    int total_steps{0};
    int steps_with_correct_reaction{0};    ///< Шаги с правильной реакцией
    bool safety_maintained{true};          ///< Безопасность сохранена
    bool degraded_mode_triggered{false};   ///< Деградированный режим был активирован
    std::vector<std::string> issues;       ///< Обнаруженные проблемы
    std::vector<std::string> observations; ///< Наблюдения
    Timestamp started_at{0};
    Timestamp completed_at{0};
};

} // namespace tb::scenarios
