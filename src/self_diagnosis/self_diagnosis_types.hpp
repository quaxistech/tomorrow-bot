#pragma once
/**
 * @file self_diagnosis_types.hpp
 * @brief Типы самодиагностики — объяснение решений, состояния системы
 *        и рекомендуемые корректирующие действия
 *
 * Модуль предоставляет:
 *   - DiagnosticType     — классификация диагностических событий
 *   - CorrectiveAction   — стандартизированные корректирующие действия
 *   - DiagnosticSeverity — уровень серьёзности события
 *   - DiagnosticFactor   — фактор, повлиявший на решение
 *   - DiagnosticRecord   — полная запись диагностики
 *   - StrategyScorecard  — агрегированная статистика по стратегии
 *   - DiagnosticSummary  — сводка диагностики за период
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace tb::self_diagnosis {

// ============================================================
// Тип диагностической записи
// ============================================================

/// Тип диагностической записи
enum class DiagnosticType {
    TradeTaken,                 ///< Объяснение совершённой сделки
    TradeDenied,                ///< Объяснение отказа от сделки
    SystemState,                ///< Диагностика состояния системы
    StrategyHealth,             ///< Здоровье стратегии
    DegradedState,              ///< Деградированное состояние
    ExecutionFailure,           ///< Ошибка исполнения ордера
    ReconciliationMismatch,     ///< Расхождение при сверке с биржей
    PortfolioConstraint,        ///< Ограничение портфеля (капитал, экспозиция)
    MarketDataDegradation,      ///< Деградация рыночных данных
    ExchangeConnectivityIncident, ///< Инцидент подключения к бирже
    StrategySuppression,        ///< Подавление стратегии (alpha decay, governance)
    RecoveryAction              ///< Действие восстановления после сбоя
};

/// Строковое представление типа
[[nodiscard]] inline std::string to_string(DiagnosticType t) {
    switch (t) {
        case DiagnosticType::TradeTaken:                  return "TradeTaken";
        case DiagnosticType::TradeDenied:                 return "TradeDenied";
        case DiagnosticType::SystemState:                 return "SystemState";
        case DiagnosticType::StrategyHealth:              return "StrategyHealth";
        case DiagnosticType::DegradedState:               return "DegradedState";
        case DiagnosticType::ExecutionFailure:            return "ExecutionFailure";
        case DiagnosticType::ReconciliationMismatch:      return "ReconciliationMismatch";
        case DiagnosticType::PortfolioConstraint:         return "PortfolioConstraint";
        case DiagnosticType::MarketDataDegradation:       return "MarketDataDegradation";
        case DiagnosticType::ExchangeConnectivityIncident:return "ExchangeConnectivityIncident";
        case DiagnosticType::StrategySuppression:         return "StrategySuppression";
        case DiagnosticType::RecoveryAction:              return "RecoveryAction";
    }
    return "Unknown";
}

// ============================================================
// Корректирующие действия
// ============================================================

/// Стандартизированное корректирующее действие, рекомендуемое диагностикой
enum class CorrectiveAction {
    Observe,          ///< Только наблюдать, действия не требуются
    SlowDown,         ///< Увеличить интервалы между ордерами
    ReduceSize,       ///< Уменьшить размер позиций
    StopEntries,      ///< Запретить новые входы, разрешить выходы
    ForceReconcile,   ///< Принудительная сверка с биржей
    HaltSymbol,       ///< Остановить торговлю по символу
    HaltSystem        ///< Остановить всю торговлю (kill switch)
};

/// Строковое представление корректирующего действия
[[nodiscard]] inline std::string to_string(CorrectiveAction a) {
    switch (a) {
        case CorrectiveAction::Observe:        return "Observe";
        case CorrectiveAction::SlowDown:       return "SlowDown";
        case CorrectiveAction::ReduceSize:     return "ReduceSize";
        case CorrectiveAction::StopEntries:    return "StopEntries";
        case CorrectiveAction::ForceReconcile: return "ForceReconcile";
        case CorrectiveAction::HaltSymbol:     return "HaltSymbol";
        case CorrectiveAction::HaltSystem:     return "HaltSystem";
    }
    return "Unknown";
}

// ============================================================
// Серьёзность события
// ============================================================

/// Уровень серьёзности диагностического события
enum class DiagnosticSeverity {
    Info,       ///< Информационное — штатная работа
    Warning,    ///< Предупреждение — возможна деградация
    Critical,   ///< Критическое — требуется немедленное действие
    Fatal       ///< Фатальное — система неработоспособна
};

/// Строковое представление серьёзности
[[nodiscard]] inline std::string to_string(DiagnosticSeverity s) {
    switch (s) {
        case DiagnosticSeverity::Info:     return "Info";
        case DiagnosticSeverity::Warning:  return "Warning";
        case DiagnosticSeverity::Critical: return "Critical";
        case DiagnosticSeverity::Fatal:    return "Fatal";
    }
    return "Unknown";
}

// ============================================================
// Фактор, повлиявший на решение
// ============================================================

/// Фактор, повлиявший на решение
struct DiagnosticFactor {
    std::string component;    ///< Компонент ("world_model", "risk", "strategy:momentum", и т.д.)
    std::string observation;  ///< Наблюдение (текст)
    double impact{0.0};       ///< Влияние [-1=категорически против, +1=категорически за]
};

// ============================================================
// Диагностическая запись
// ============================================================

/// Диагностическая запись — полное объяснение решения или состояния
struct DiagnosticRecord {
    uint64_t diagnostic_id{0};
    DiagnosticType type{DiagnosticType::SystemState};
    DiagnosticSeverity severity{DiagnosticSeverity::Info};
    CorrelationId correlation_id{""};
    Symbol symbol{""};
    Timestamp created_at{0};

    // Контекст
    std::string world_state;
    std::string regime;
    std::string uncertainty_level;

    // Факторы, повлиявшие на решение
    std::vector<DiagnosticFactor> factors;

    // Вердикт
    std::string verdict;              ///< Краткий вердикт
    std::string human_summary;        ///< Человекочитаемое объяснение
    std::string machine_json;         ///< Машиночитаемый JSON

    // Рекомендуемое корректирующее действие
    CorrectiveAction recommended_action{CorrectiveAction::Observe};

    // Метаданные
    std::string strategy_id;
    std::string risk_verdict;
    bool trade_executed{false};
};

// ============================================================
// Агрегированная статистика — Scorecard
// ============================================================

/// Агрегированная статистика диагностики по стратегии
struct StrategyScorecard {
    std::string strategy_id;
    uint64_t total_signals{0};         ///< Всего сигналов
    uint64_t trades_taken{0};          ///< Совершённых сделок
    uint64_t trades_denied{0};         ///< Отклонённых сделок
    uint64_t risk_reductions{0};       ///< Уменьшений размера
    uint64_t execution_failures{0};    ///< Ошибок исполнения
    uint64_t suppressions{0};          ///< Подавлений стратегии
    double avg_conviction{0.0};        ///< Средняя убеждённость
    double denial_rate{0.0};           ///< Доля отклонений (0.0–1.0)
};

/// Сводка диагностики за период
struct DiagnosticSummary {
    uint64_t total_records{0};                   ///< Всего диагностических записей
    uint64_t info_count{0};                      ///< Информационных
    uint64_t warning_count{0};                   ///< Предупреждений
    uint64_t critical_count{0};                  ///< Критических
    uint64_t fatal_count{0};                     ///< Фатальных
    std::unordered_map<std::string, StrategyScorecard> strategy_scorecards;
    std::unordered_map<std::string, uint64_t> type_counts;  ///< Счётчики по типам
};

} // namespace tb::self_diagnosis
