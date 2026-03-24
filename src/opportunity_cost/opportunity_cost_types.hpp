#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::opportunity_cost {

/// Оценка торговой возможности
struct OpportunityScore {
    double score{0.0};                 ///< Общий балл возможности [0, 1]
    double expected_return_bps{0.0};   ///< Ожидаемый доход (базисные пункты)
    double execution_cost_bps{0.0};    ///< Стоимость исполнения
    double net_expected_bps{0.0};      ///< Чистый ожидаемый доход
    double capital_efficiency{0.0};    ///< Эффективность использования капитала [0,1]
};

/// Решение по возможности
enum class OpportunityAction {
    Execute,     ///< Исполнить сейчас
    Defer,       ///< Отложить — ожидается лучшая возможность
    Suppress,    ///< Подавить — не стоит капитала
    Upgrade      ///< Заменить худшую существующую позицию
};

/// Полный результат анализа opportunity cost
struct OpportunityCostResult {
    OpportunityScore score;
    OpportunityAction action{OpportunityAction::Execute};
    int rank{0};                        ///< Ранг среди текущих кандидатов (1 = лучший)
    double budget_utilization{0.0};     ///< Текущая утилизация бюджета [0, 1]
    std::string rationale;
    Timestamp computed_at{Timestamp(0)};
};

/// Преобразование действия в строку
std::string to_string(OpportunityAction action);

} // namespace tb::opportunity_cost
