#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace tb::portfolio {

/// Одна открытая позиция
struct Position {
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};                    ///< Направление позиции
    Quantity size{Quantity(0.0)};             ///< Текущий размер
    Price avg_entry_price{Price(0.0)};       ///< Средняя цена входа
    Price current_price{Price(0.0)};         ///< Текущая рыночная цена
    NotionalValue notional{NotionalValue(0.0)}; ///< Номинальная стоимость
    double unrealized_pnl{0.0};              ///< Нереализованная прибыль/убыток (USD)
    double unrealized_pnl_pct{0.0};          ///< Нереализованная P&L в процентах
    StrategyId strategy_id{StrategyId("")};  ///< Стратегия, открывшая позицию
    Timestamp opened_at{Timestamp(0)};
    Timestamp updated_at{Timestamp(0)};
};

/// Направленная экспозиция
struct ExposureSummary {
    double gross_exposure{0.0};     ///< Суммарный модуль экспозиции (USD)
    double net_exposure{0.0};       ///< Чистая экспозиция (long - short) (USD)
    double long_exposure{0.0};      ///< Суммарная длинная экспозиция (USD)
    double short_exposure{0.0};     ///< Суммарная короткая экспозиция (USD)
    double exposure_pct{0.0};       ///< Экспозиция как % от капитала
    int open_positions_count{0};
};

/// Сводка по P&L
struct PnlSummary {
    double realized_pnl_today{0.0};     ///< Реализованная прибыль за сегодня (USD)
    double unrealized_pnl{0.0};         ///< Нереализованная прибыль (USD)
    double total_pnl{0.0};              ///< Общая P&L (realized + unrealized)
    double peak_equity{0.0};            ///< Пик капитала за сегодня
    double current_drawdown_pct{0.0};   ///< Текущая просадка (%)
    int trades_today{0};                ///< Количество сделок за сегодня
    int consecutive_losses{0};          ///< Серия подряд убыточных сделок
};

/// Полный снимок портфеля
struct PortfolioSnapshot {
    std::vector<Position> positions;
    ExposureSummary exposure;
    PnlSummary pnl;
    double total_capital{10000.0};      ///< Общий капитал (USD)
    double available_capital{10000.0};  ///< Доступный капитал (USD)
    Timestamp computed_at{Timestamp(0)};
};

} // namespace tb::portfolio
