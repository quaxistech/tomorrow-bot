/**
 * @file governance_types.hpp
 * @brief Типы для слоя управления и аудита
 *
 * Определяет аудиторские события, реестр стратегий и snapshot governance.
 */
#pragma once

#include "common/types.hpp"

#include <string>
#include <vector>

namespace tb::governance {

/// Тип аудиторского события
enum class AuditEventType {
    StrategyRegistered,     ///< Стратегия зарегистрирована
    StrategyVersionChanged, ///< Версия стратегии изменена
    StrategyEnabled,        ///< Стратегия включена
    StrategyDisabled,       ///< Стратегия выключена
    ConfigChanged,          ///< Конфигурация изменена
    ModeTransition,         ///< Переход режима работы
    KillSwitchActivated,    ///< Kill switch активирован
    KillSwitchDeactivated,  ///< Kill switch деактивирован
    OperatorIntervention,   ///< Вмешательство оператора
    ChallengerPromoted,     ///< Challenger повышен
    ChallengerRejected,     ///< Challenger отклонён
    SafeModeEntered,        ///< Вход в безопасный режим
    SafeModeExited,         ///< Выход из безопасного режима
    SystemStartup,          ///< Запуск системы
    SystemShutdown          ///< Остановка системы
};

/// Преобразование типа события в строку
inline std::string to_string(AuditEventType t) {
    switch (t) {
        case AuditEventType::StrategyRegistered:     return "StrategyRegistered";
        case AuditEventType::StrategyVersionChanged: return "StrategyVersionChanged";
        case AuditEventType::StrategyEnabled:        return "StrategyEnabled";
        case AuditEventType::StrategyDisabled:       return "StrategyDisabled";
        case AuditEventType::ConfigChanged:          return "ConfigChanged";
        case AuditEventType::ModeTransition:         return "ModeTransition";
        case AuditEventType::KillSwitchActivated:    return "KillSwitchActivated";
        case AuditEventType::KillSwitchDeactivated:  return "KillSwitchDeactivated";
        case AuditEventType::OperatorIntervention:   return "OperatorIntervention";
        case AuditEventType::ChallengerPromoted:     return "ChallengerPromoted";
        case AuditEventType::ChallengerRejected:     return "ChallengerRejected";
        case AuditEventType::SafeModeEntered:        return "SafeModeEntered";
        case AuditEventType::SafeModeExited:         return "SafeModeExited";
        case AuditEventType::SystemStartup:          return "SystemStartup";
        case AuditEventType::SystemShutdown:         return "SystemShutdown";
    }
    return "Unknown";
}

/// Запись аудита
struct AuditRecord {
    uint64_t audit_id{0};
    AuditEventType type;
    Timestamp timestamp{0};
    std::string actor;           ///< Кто инициировал ("system", "operator", "alpha_decay")
    std::string target;          ///< Цель ("strategy:momentum_v2", "config", "kill_switch")
    std::string details;         ///< Подробности
    ConfigHash config_hash{""}; ///< Хэш конфигурации на момент события
    std::string metadata_json;   ///< Дополнительные метаданные (JSON)
};

/// Запись реестра стратегий
struct StrategyRegistryEntry {
    StrategyId strategy_id{""};
    StrategyVersion version{0};
    bool enabled{true};
    bool is_champion{false};
    bool is_shadow{false};
    TradingMode mode{TradingMode::Paper};
    Timestamp registered_at{0};
    Timestamp last_updated{0};
    ConfigHash config_hash{""};
};

/// Снимок governance состояния
struct GovernanceSnapshot {
    std::vector<StrategyRegistryEntry> strategy_registry;
    std::vector<AuditRecord> recent_audit;  ///< Последние N записей
    ConfigHash current_config_hash{""};
    std::string runtime_version;
    TradingMode current_mode{TradingMode::Paper};
    bool kill_switch_active{false};
    bool safe_mode_active{false};
    Timestamp snapshot_at{0};
};

} // namespace tb::governance
