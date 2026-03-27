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
    double fill_probability{0.5};        ///< Вероятность заполнения [0,1] — data-driven
    double adverse_selection_risk{0.0};  ///< Риск неблагоприятного отбора [0,1]
    double total_cost_bps{0.0};          ///< Полная стоимость исполнения
};

/// План нарезки ордера (для крупных ордеров)
struct SlicePlan {
    int num_slices{1};               ///< Количество частей
    double slice_interval_ms{100.0}; ///< Интервал между частями (мс)
    double first_slice_pct{1.0};     ///< Доля первой части [0,1]
    std::string rationale;
};

/// Покомпонентная декомпозиция решения для прозрачности и отладки.
/// Производственный стандарт: каждый фактор, повлиявший на решение,
/// должен быть зафиксирован и доступен для аудита.
struct DecisionFactors {
    // ── Компоненты токсичности потока [0..1] ──
    double spread_toxicity{0.0};         ///< Вклад ширины спреда в токсичность
    double flow_toxicity{0.0};           ///< Вклад aggressive_flow в токсичность
    double book_instability_score{0.0};  ///< Вклад нестабильности стакана
    double vpin_toxicity{0.0};           ///< Вклад VPIN (если доступен)
    double adverse_selection_score{0.0}; ///< Итоговый балл неблагоприятного отбора [0..1]

    // ── Дисбаланс стакана относительно направления сделки ──
    double directional_imbalance{0.0};   ///< [-1=против нас, +1=в нашу пользу]
    bool   imbalance_used{false};        ///< Был ли дисбаланс стакана доступен?

    // ── Декомпозиция срочности ──
    double urgency_base{0.0};            ///< Базовая срочность из TradeIntent [0..1]
    double urgency_vol_adj{0.0};         ///< Поправка за волатильность
    double urgency_momentum_adj{0.0};    ///< Поправка за моментум
    double urgency_cusum_adj{0.0};       ///< Поправка за сигнал смены режима (CUSUM)
    double urgency_tod_adj{0.0};         ///< Поправка за время торговой сессии

    // ── Качество данных ──
    bool vpin_used{false};               ///< Использован ли VPIN в расчёте?
    bool weighted_mid_used{false};       ///< Использована ли взвешенная средняя цена?
    bool features_complete{true};        ///< Были ли все фичи валидны при принятии решения?
};

/// Полный результат анализа исполнения
struct ExecutionAlphaResult {
    ExecutionStyle recommended_style{ExecutionStyle::Passive};
    double urgency_score{0.0};                     ///< Срочность [0=не срочно, 1=немедленно]
    ExecutionQualityEstimate quality;
    std::optional<Price> recommended_limit_price;  ///< Рекомендуемая лимитная цена
    std::optional<SlicePlan> slice_plan;
    bool should_execute{true};                     ///< false = условия слишком неблагоприятные
    std::string rationale;
    Timestamp computed_at{Timestamp(0)};
    DecisionFactors decision_factors;              ///< Полная аудит-трасса факторов решения
};

/// Преобразование стиля исполнения в строку
std::string to_string(ExecutionStyle style);

} // namespace tb::execution_alpha
