#pragma once
/**
 * @file champion_challenger_types.hpp
 * @brief Типы Champion-Challenger A/B тестирования стратегий
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <map>

namespace tb::champion_challenger {

/// Статус челленджера
enum class ChallengerStatus {
    Registered,    ///< Зарегистрирован
    Evaluating,    ///< В процессе оценки
    Promoted,      ///< Повышен до champion
    Rejected,      ///< Отклонён
    Retired        ///< Снят с оценки
};

/// Строковое представление статуса
[[nodiscard]] inline std::string to_string(ChallengerStatus s) {
    switch (s) {
        case ChallengerStatus::Registered: return "Registered";
        case ChallengerStatus::Evaluating: return "Evaluating";
        case ChallengerStatus::Promoted:   return "Promoted";
        case ChallengerStatus::Rejected:   return "Rejected";
        case ChallengerStatus::Retired:    return "Retired";
    }
    return "Unknown";
}

/// Метрики сравнения champion vs challenger
struct ComparisonMetrics {
    double hypothetical_pnl_bps{0.0};      ///< Гипотетический P&L (bps)
    double signal_quality{0.0};            ///< Качество сигналов [0, 1]
    int decision_count{0};                 ///< Количество решений
    double avg_conviction{0.0};            ///< Средняя уверенность
    std::map<std::string, double> regime_pnl; ///< P&L по режимам
    double execution_quality{0.0};         ///< Качество исполнения [0, 1]
    Timestamp evaluation_start{0};
    Timestamp evaluation_end{0};
    int profitable_count{0};               ///< Прибыльные решения
};

/// Запись челленджера
struct ChallengerEntry {
    StrategyId challenger_id{""};
    StrategyId champion_id{""};             ///< ID champion'a
    StrategyVersion version{0};
    ChallengerStatus status{ChallengerStatus::Registered};
    ComparisonMetrics champion_metrics;     ///< Метрики champion'a за период
    ComparisonMetrics challenger_metrics;   ///< Метрики challenger'a за период
    Timestamp registered_at{0};
    std::string promotion_reason;
    std::string rejection_reason;
};

/// Отчёт сравнения champion vs challenger
struct ChampionChallengerReport {
    StrategyId champion_id{""};
    std::vector<ChallengerEntry> challengers;
    Timestamp computed_at{0};
};

/// Конфигурация champion-challenger
struct ChampionChallengerConfig {
    int min_evaluation_trades{50};         ///< Мин. сделок для оценки
    double promotion_threshold{0.2};       ///< +20% лучше champion → promote
    double rejection_threshold{-0.1};      ///< -10% хуже → reject
};

} // namespace tb::champion_challenger
