#pragma once
/**
 * @file self_diagnosis_types.hpp
 * @brief Типы самодиагностики — объяснение решений и состояния системы
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::self_diagnosis {

/// Тип диагностической записи
enum class DiagnosticType {
    TradeTaken,       ///< Объяснение совершённой сделки
    TradeDenied,      ///< Объяснение отказа от сделки
    SystemState,      ///< Диагностика состояния системы
    StrategyHealth,   ///< Здоровье стратегии
    DegradedState     ///< Деградированное состояние
};

/// Строковое представление типа
[[nodiscard]] inline std::string to_string(DiagnosticType t) {
    switch (t) {
        case DiagnosticType::TradeTaken:     return "TradeTaken";
        case DiagnosticType::TradeDenied:    return "TradeDenied";
        case DiagnosticType::SystemState:    return "SystemState";
        case DiagnosticType::StrategyHealth: return "StrategyHealth";
        case DiagnosticType::DegradedState:  return "DegradedState";
    }
    return "Unknown";
}

/// Фактор, повлиявший на решение
struct DiagnosticFactor {
    std::string component;    ///< Компонент ("world_model", "risk", "strategy:momentum", и т.д.)
    std::string observation;  ///< Наблюдение (текст)
    double impact{0.0};       ///< Влияние [-1=категорически против, +1=категорически за]
};

/// Диагностическая запись — полное объяснение решения или состояния
struct DiagnosticRecord {
    uint64_t diagnostic_id{0};
    DiagnosticType type{DiagnosticType::SystemState};
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

    // Метаданные
    std::string strategy_id;
    std::string risk_verdict;
    bool trade_executed{false};
};

} // namespace tb::self_diagnosis
