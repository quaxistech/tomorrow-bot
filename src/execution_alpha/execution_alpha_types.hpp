#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::execution_alpha {

/// Стиль исполнения ордера
enum class ExecutionStyle {
    Passive,       ///< Лимитный ордер, ожидание заполнения (maker)
    Aggressive,    ///< Рыночный ордер, немедленное исполнение (taker)
    Hybrid,        ///< Начать пассивно, перейти к агрессивному при необходимости
    PostOnly,      ///< Только maker, отмена при пересечении
    NoExecution    ///< Условия слишком токсичны — отказ от исполнения
};

/// Оценка качества исполнения
struct ExecutionQualityEstimate {
    double spread_cost_bps{0.0};         ///< Стоимость спреда (базисные пункты)
    double estimated_slippage_bps{0.0};  ///< Ожидаемое проскальзывание
    double fill_probability{0.5};         ///< Вероятность заполнения [0,1]
    double adverse_selection_risk{0.0};   ///< Риск неблагоприятного отбора [0,1]
    double total_cost_bps{0.0};          ///< Полная стоимость исполнения
};

/// План нарезки ордера (для крупных ордеров)
struct SlicePlan {
    int num_slices{1};                   ///< Количество частей
    double slice_interval_ms{100.0};     ///< Интервал между частями
    double first_slice_pct{1.0};         ///< Доля первой части [0,1]
    std::string rationale;
};

/// Полный результат анализа исполнения
struct ExecutionAlphaResult {
    ExecutionStyle recommended_style{ExecutionStyle::Passive};
    double urgency_score{0.0};                    ///< Срочность [0=не срочно, 1=немедленно]
    ExecutionQualityEstimate quality;
    std::optional<Price> recommended_limit_price;  ///< Рекомендуемая лимитная цена
    std::optional<SlicePlan> slice_plan;
    bool should_execute{true};                     ///< false = условия слишком плохие
    std::string rationale;
    Timestamp computed_at{Timestamp(0)};
};

/// Преобразование стиля исполнения в строку
std::string to_string(ExecutionStyle style);

} // namespace tb::execution_alpha
