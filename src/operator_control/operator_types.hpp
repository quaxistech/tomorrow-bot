/**
 * @file operator_types.hpp
 * @brief Типы для панели управления оператора
 *
 * Определяет команды оператора, запросы и ответы.
 */
#pragma once

#include "common/types.hpp"

#include <string>

namespace tb::operator_control {

/// Тип команды оператора
enum class OperatorCommand {
    EnableStrategy,         ///< Включить стратегию
    DisableStrategy,        ///< Выключить стратегию
    MoveToShadow,           ///< Перевести в теневой режим
    MoveToLive,             ///< Перевести в live-режим
    ActivateKillSwitch,     ///< Активировать kill switch
    DeactivateKillSwitch,   ///< Деактивировать kill switch
    EnterSafeMode,          ///< Войти в безопасный режим
    ExitSafeMode,           ///< Выйти из безопасного режима
    InspectHealth,          ///< Просмотр здоровья системы
    InspectRegime,          ///< Просмотр режима рынка
    InspectPortfolio,       ///< Просмотр портфеля
    InspectGovernance,      ///< Просмотр governance
    ForceFlush              ///< Принудительный сброс данных
};

/// Преобразование команды в строку
inline std::string to_string(OperatorCommand cmd) {
    switch (cmd) {
        case OperatorCommand::EnableStrategy:       return "EnableStrategy";
        case OperatorCommand::DisableStrategy:      return "DisableStrategy";
        case OperatorCommand::MoveToShadow:         return "MoveToShadow";
        case OperatorCommand::MoveToLive:           return "MoveToLive";
        case OperatorCommand::ActivateKillSwitch:   return "ActivateKillSwitch";
        case OperatorCommand::DeactivateKillSwitch: return "DeactivateKillSwitch";
        case OperatorCommand::EnterSafeMode:        return "EnterSafeMode";
        case OperatorCommand::ExitSafeMode:         return "ExitSafeMode";
        case OperatorCommand::InspectHealth:        return "InspectHealth";
        case OperatorCommand::InspectRegime:        return "InspectRegime";
        case OperatorCommand::InspectPortfolio:     return "InspectPortfolio";
        case OperatorCommand::InspectGovernance:    return "InspectGovernance";
        case OperatorCommand::ForceFlush:           return "ForceFlush";
    }
    return "Unknown";
}

/// Запрос оператора
struct OperatorRequest {
    OperatorCommand command;
    std::string operator_id;     ///< ID оператора
    std::string target;          ///< Цель команды (strategy_id и т.д.)
    std::string reason;          ///< Причина действия
    Timestamp requested_at{0};
};

/// Результат выполнения команды
struct OperatorResponse {
    bool success{false};
    std::string message;
    std::string details_json;    ///< Детали в JSON (для inspect-команд)
    Timestamp executed_at{0};
};

} // namespace tb::operator_control
