#pragma once
#include "common/types.hpp"
#include "common/constants.hpp"
#include "regime/regime_types.hpp"
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace tb::portfolio_allocator {

/// Иерархия бюджетов (USDT-M futures)
struct BudgetHierarchy {
    double global_budget{10000.0};          ///< Глобальный бюджет (USDT) — fallback при total_capital <= 0
    double symbol_budget_pct{0.1};          ///< Доля бюджета на символ [0,1]
};

/// Торговые правила биржи для символа
struct ExchangeFilters {
    Symbol symbol{Symbol("")};
    double min_quantity{0.0};         ///< Минимальный объём ордера
    double max_quantity{1e12};        ///< Максимальный объём ордера
    double quantity_step{0.0};        ///< Шаг объёма (lot size)
    double min_notional{1.0};         ///< Минимальный нотионал (USDT)
    double tick_size{0.01};           ///< Шаг цены
    int quantity_precision{8};        ///< Количество знаков после запятой для qty
    int price_precision{2};           ///< Количество знаков после запятой для price
    double taker_fee_pct{tb::common::fees::kDefaultTakerFeePct};      ///< Комиссия тейкера (0.1%)
    double maker_fee_pct{tb::common::fees::kDefaultMakerFeePct};     ///< Комиссия мейкера (0.08%)
};

/// Полный контекст для принятия решения по размеру позиции
struct AllocationContext {
    // Рыночные данные
    double realized_vol_annual{0.0};          ///< Реализованная годовая волатильность
    regime::DetailedRegime regime{regime::DetailedRegime::Undefined};
    double win_rate{0.5};                     ///< Доля выигрышных сделок [0,1]
    double avg_win_loss_ratio{1.5};           ///< Средний ratio выигрыш/проигрыш

    // Контекст ликвидности
    double avg_daily_volume{0.0};             ///< Средний дневной объём (USDT)
    double current_spread_bps{0.0};           ///< Текущий спред (basis points)
    double book_depth_notional{0.0};          ///< Глубина стакана (USDT)

    // Контекст портфеля
    double current_drawdown_pct{0.0};         ///< Текущая просадка (%)
    int consecutive_losses{0};                ///< Серия убытков подряд

    // Exchange filters (если заданы)
    std::optional<ExchangeFilters> exchange_filters;
};

/// Результат проверки одного ограничения
struct ConstraintDecision {
    std::string constraint_name;              ///< Имя ограничения
    double limit_value{0.0};                  ///< Значение лимита
    double input_value{0.0};                  ///< Входное значение (до применения)
    double output_value{0.0};                 ///< Выходное значение (после применения)
    bool was_binding{false};                  ///< Ограничение было активным
    std::string details;
};

/// Результат аллокации для конкретного ордера
struct SizingResult {
    Quantity approved_quantity{Quantity(0.0)};
    NotionalValue approved_notional{NotionalValue(0.0)};
    double size_as_pct_of_capital{0.0};
    bool was_reduced{false};          ///< Размер был уменьшен из-за ограничений
    std::string reduction_reason;
    bool approved{true};

    // Аудит-трейл ограничений
    std::vector<ConstraintDecision> constraint_audit;
    double fee_adjusted_notional{0.0};        ///< Нотионал с учётом комиссии
    double expected_fee{0.0};                 ///< Ожидаемая комиссия
};

// ---------------------------------------------------------------------------
// Утилиты для exchange filters
// ---------------------------------------------------------------------------

/// Округлить объём до шага биржи (floor)
inline double round_quantity_down(double qty, const ExchangeFilters& filters) {
    if (filters.quantity_step <= 0.0) return qty;
    double steps = std::floor(qty / filters.quantity_step);
    return steps * filters.quantity_step;
}

/// Округлить цену до шага биржи
inline double round_price(double price, const ExchangeFilters& filters) {
    if (filters.tick_size <= 0.0) return price;
    return std::round(price / filters.tick_size) * filters.tick_size;
}

/// Проверить допустимость ордера по exchange filters
inline bool validate_order_filters(double qty, double notional, const ExchangeFilters& filters) {
    if (qty < filters.min_quantity) return false;
    if (qty > filters.max_quantity) return false;
    if (notional < filters.min_notional) return false;
    return true;
}

} // namespace tb::portfolio_allocator
