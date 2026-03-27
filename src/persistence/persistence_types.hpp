#pragma once
/**
 * @file persistence_types.hpp
 * @brief Типы слоя персистентности — журнал событий и снимки состояний
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace tb::persistence {

/// Тип записи в журнале событий
enum class JournalEntryType {
    MarketEvent,        ///< Рыночное событие
    DecisionTrace,      ///< Трассировка решения
    RiskDecision,       ///< Решение риск-менеджера
    OrderEvent,         ///< Событие ордера
    PortfolioChange,    ///< Изменение портфеля
    StrategySignal,     ///< Сигнал стратегии
    SystemEvent,        ///< Системное событие
    TelemetrySnapshot,  ///< Снимок телеметрии
    GovernanceEvent,    ///< Событие governance-слоя
    DiagnosticEvent,    ///< Событие диагностики системы
    ShadowEvent         ///< Событие shadow trading подсистемы
};

/// Строковое представление типа записи
[[nodiscard]] inline std::string to_string(JournalEntryType t) {
    switch (t) {
        case JournalEntryType::MarketEvent:       return "MarketEvent";
        case JournalEntryType::DecisionTrace:     return "DecisionTrace";
        case JournalEntryType::RiskDecision:      return "RiskDecision";
        case JournalEntryType::OrderEvent:        return "OrderEvent";
        case JournalEntryType::PortfolioChange:   return "PortfolioChange";
        case JournalEntryType::StrategySignal:    return "StrategySignal";
        case JournalEntryType::SystemEvent:       return "SystemEvent";
        case JournalEntryType::TelemetrySnapshot: return "TelemetrySnapshot";
        case JournalEntryType::GovernanceEvent:   return "GovernanceEvent";
        case JournalEntryType::DiagnosticEvent:   return "DiagnosticEvent";
        case JournalEntryType::ShadowEvent:       return "ShadowEvent";
    }
    return "Unknown";
}

/// Запись в журнале событий (append-only)
struct JournalEntry {
    uint64_t sequence_id{0};                      ///< Монотонный идентификатор
    JournalEntryType type{JournalEntryType::SystemEvent};
    Timestamp timestamp{0};                       ///< Время события
    CorrelationId correlation_id{""};             ///< ID корреляции запроса
    StrategyId strategy_id{""};                   ///< ID стратегии (если применимо)
    ConfigHash config_hash{""};                   ///< Хэш конфигурации
    std::string payload_json;                     ///< Сериализованные данные (JSON)
};

/// Тип снимка состояния
enum class SnapshotType {
    Portfolio,       ///< Снимок портфеля
    RiskCounters,    ///< Счётчики риска
    StrategyMeta,    ///< Метаданные стратегий
    WorldState,      ///< Состояние мира
    FullSystem,       ///< Полный снимок системы
    GovernanceState   ///< Снимок governance-состояния
};

/// Строковое представление типа снимка
[[nodiscard]] inline std::string to_string(SnapshotType t) {
    switch (t) {
        case SnapshotType::Portfolio:    return "Portfolio";
        case SnapshotType::RiskCounters: return "RiskCounters";
        case SnapshotType::StrategyMeta: return "StrategyMeta";
        case SnapshotType::WorldState:   return "WorldState";
        case SnapshotType::FullSystem:       return "FullSystem";
        case SnapshotType::GovernanceState:  return "GovernanceState";
    }
    return "Unknown";
}

/// Запись снимка состояния
struct SnapshotEntry {
    uint64_t snapshot_id{0};           ///< Уникальный ID снимка
    SnapshotType type{SnapshotType::FullSystem};
    Timestamp created_at{0};           ///< Время создания
    ConfigHash config_hash{""};        ///< Хэш конфигурации
    std::string payload_json;          ///< Сериализованные данные (JSON)
};

/// Конфигурация персистентности
struct PersistenceConfig {
    std::string data_dir{"./data"};     ///< Базовая директория хранения
    std::string journal_subdir{"journal"};  ///< Поддиректория журнала
    std::string snapshot_subdir{"snapshots"}; ///< Поддиректория снимков
    bool enabled{true};                 ///< Включена ли персистентность
    int flush_interval_ms{1000};        ///< Интервал сброса на диск (мс)
};

} // namespace tb::persistence
