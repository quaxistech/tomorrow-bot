/**
 * @file governance_types.hpp
 * @brief Типы для слоя управления и аудита
 *
 * Определяет аудиторские события, реестр стратегий, snapshot governance,
 * режимы остановки, инцидентные состояния и результат governance gate.
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
    SystemShutdown,         ///< Остановка системы
    GovernanceGateDenied,   ///< Пайплайн отклонён governance gate
    HaltModeChanged,        ///< Переход режима остановки
    IncidentStateChanged,   ///< Переход инцидентного FSM
    StrategyDraining,       ///< Стратегия входит в режим дренажа
    StrategyRetired,        ///< Стратегия выведена из эксплуатации
    AutoActionRequested,    ///< Автодействие от alpha_decay / self_diagnosis
    PolicyDenied,           ///< Политика отклонила действие
    ConfigRollback,         ///< Откат конфигурации выполнен
    ShadowDecisionRecorded, ///< Записано теневое решение
    ShadowComparisonGenerated ///< Сгенерировано shadow vs live сравнение
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
        case AuditEventType::GovernanceGateDenied:   return "GovernanceGateDenied";
        case AuditEventType::HaltModeChanged:        return "HaltModeChanged";
        case AuditEventType::IncidentStateChanged:   return "IncidentStateChanged";
        case AuditEventType::StrategyDraining:       return "StrategyDraining";
        case AuditEventType::StrategyRetired:        return "StrategyRetired";
        case AuditEventType::AutoActionRequested:    return "AutoActionRequested";
        case AuditEventType::PolicyDenied:           return "PolicyDenied";
        case AuditEventType::ConfigRollback:         return "ConfigRollback";
        case AuditEventType::ShadowDecisionRecorded: return "ShadowDecisionRecorded";
        case AuditEventType::ShadowComparisonGenerated: return "ShadowComparisonGenerated";
    }
    return "Unknown";
}

/// Режим остановки торговли
enum class HaltMode {
    None,           ///< Нет ограничений
    NoNewEntries,   ///< Запрет новых позиций
    ReduceOnly,     ///< Только уменьшение позиций
    CloseOnly,      ///< Только закрытие позиций
    HardHalt        ///< Полная остановка торговли
};

/// Преобразование режима остановки в строку
inline std::string to_string(HaltMode m) {
    switch (m) {
        case HaltMode::None:         return "None";
        case HaltMode::NoNewEntries: return "NoNewEntries";
        case HaltMode::ReduceOnly:   return "ReduceOnly";
        case HaltMode::CloseOnly:    return "CloseOnly";
        case HaltMode::HardHalt:     return "HardHalt";
    }
    return "Unknown";
}

/// Состояние инцидентного FSM
enum class IncidentState {
    Normal,      ///< Нормальная работа
    Degraded,    ///< Деградация (частичные сбои)
    Restricted,  ///< Ограниченный режим
    Halted,      ///< Полная остановка
    Recovering   ///< Восстановление после инцидента
};

/// Преобразование инцидентного состояния в строку
inline std::string to_string(IncidentState s) {
    switch (s) {
        case IncidentState::Normal:     return "Normal";
        case IncidentState::Degraded:   return "Degraded";
        case IncidentState::Restricted: return "Restricted";
        case IncidentState::Halted:     return "Halted";
        case IncidentState::Recovering: return "Recovering";
    }
    return "Unknown";
}

/// Состояние жизненного цикла стратегии
enum class StrategyLifecycleState {
    Registered, ///< Зарегистрирована, ещё не запущена
    Shadow,     ///< Теневой режим (наблюдение)
    Candidate,  ///< Кандидат на продвижение
    Live,       ///< Боевой режим
    Draining,   ///< Дренаж (закрытие позиций)
    Disabled,   ///< Выключена
    Retired     ///< Выведена из эксплуатации
};

/// Преобразование состояния жизненного цикла в строку
inline std::string to_string(StrategyLifecycleState s) {
    switch (s) {
        case StrategyLifecycleState::Registered: return "Registered";
        case StrategyLifecycleState::Shadow:     return "Shadow";
        case StrategyLifecycleState::Candidate:  return "Candidate";
        case StrategyLifecycleState::Live:       return "Live";
        case StrategyLifecycleState::Draining:   return "Draining";
        case StrategyLifecycleState::Disabled:   return "Disabled";
        case StrategyLifecycleState::Retired:    return "Retired";
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
    std::string subsystem;       ///< Подсистема-источник события
    std::string severity;        ///< Уровень серьёзности ("info", "warn", "critical")
    std::string previous_state;  ///< Состояние до изменения
    std::string new_state;       ///< Состояние после изменения
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
    StrategyLifecycleState lifecycle_state{StrategyLifecycleState::Registered};
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
    HaltMode halt_mode{HaltMode::None};
    IncidentState incident_state{IncidentState::Normal};
    std::string incident_reason;              ///< Причина текущего инцидента
};

/// Результат проверки governance gate
struct GovernanceGateResult {
    bool trading_allowed{false};              ///< Торговля разрешена
    bool new_entries_allowed{false};          ///< Новые позиции разрешены
    bool close_only{false};                   ///< Только закрытие позиций
    std::string denial_reason;                ///< Причина отказа
    HaltMode current_halt{HaltMode::None};
    IncidentState current_incident{IncidentState::Normal};
};

} // namespace tb::governance
