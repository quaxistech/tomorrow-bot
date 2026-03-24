#pragma once
#include "common/types.hpp"
#include <string>

namespace tb::portfolio_allocator {

/// Иерархия бюджетов
struct BudgetHierarchy {
    double global_budget{10000.0};          ///< Глобальный бюджет (USD)
    double global_utilization_pct{0.0};     ///< Использование глобального бюджета [0,1]
    double regime_budget_pct{0.5};          ///< Доля бюджета для текущего режима [0,1]
    double strategy_budget_pct{0.2};        ///< Доля бюджета для стратегии [0,1]
    double symbol_budget_pct{0.1};          ///< Доля бюджета для символа [0,1]
};

/// Результат аллокации для конкретного ордера
struct SizingResult {
    Quantity approved_quantity{Quantity(0.0)};
    NotionalValue approved_notional{NotionalValue(0.0)};
    double size_as_pct_of_capital{0.0};
    bool was_reduced{false};          ///< Размер был уменьшен из-за ограничений
    std::string reduction_reason;
    bool approved{true};
};

} // namespace tb::portfolio_allocator
