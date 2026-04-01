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
    double accumulated_funding{0.0};         ///< Накопленные funding payments (USD) - для фьючерсов
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

/// Учёт комиссий по сделке
struct FeeRecord {
    double buy_fee{0.0};      ///< Комиссия на покупку (USD)
    double sell_fee{0.0};     ///< Комиссия на продажу (USD)
    double total_fees{0.0};   ///< Суммарная комиссия (USD)
};

/// Институциональный учёт cash-баланса
struct CashLedger {
    double total_cash{0.0};              ///< Полный cash-баланс (USDT)
    double available_cash{0.0};          ///< Доступный cash (за вычетом резервов)
    double reserved_for_orders{0.0};     ///< Зарезервировано под активные BUY-ордера
    double fees_accrued_today{0.0};      ///< Комиссии за сегодня
    double realized_pnl_gross{0.0};      ///< Реализованная P&L без учёта комиссий
    double realized_pnl_net{0.0};        ///< Реализованная P&L с учётом комиссий
    double pending_buy_notional{0.0};    ///< Суммарный нотионал активных BUY-ордеров
    double pending_sell_notional{0.0};   ///< Суммарный нотионал активных SELL-ордеров
};

/// Информация о pending-ордере для учёта резервов
struct PendingOrderInfo {
    OrderId order_id{OrderId("")};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    Quantity quantity{Quantity(0.0)};
    Price expected_price{Price(0.0)};
    double reserved_cash{0.0};           ///< Cash зарезервированный под этот ордер (BUY only)
    double reserved_notional{0.0};       ///< Нотионал ордера
    double estimated_fee{0.0};           ///< Ожидаемая комиссия
    Timestamp submitted_at{Timestamp(0)};
    StrategyId strategy_id{StrategyId("")};
};

/// Типы событий портфеля (event sourcing)
enum class PortfolioEventType {
    PositionOpened,
    PositionClosed,
    PositionUpdated,
    PriceUpdated,
    CashReserved,
    CashReleased,
    FeeCharged,
    CapitalSynced,
    DailyReset,
    ReconciliationAdjustment
};

/// Событие портфеля для аудит-лога
struct PortfolioEvent {
    PortfolioEventType type{PortfolioEventType::PriceUpdated};
    Symbol symbol{Symbol("")};
    double amount{0.0};              ///< Сумма операции
    double balance_after{0.0};       ///< Баланс после операции
    std::string details;
    Timestamp occurred_at{Timestamp(0)};
    OrderId order_id{OrderId("")};   ///< Связанный ордер (если есть)
};

/// Полный снимок портфеля
struct PortfolioSnapshot {
    std::vector<Position> positions;
    ExposureSummary exposure;
    PnlSummary pnl;
    double total_capital{10000.0};      ///< Общий капитал (USD)
    double available_capital{10000.0};  ///< Доступный капитал (USD)
    Timestamp computed_at{Timestamp(0)};
    CashLedger cash;                                        ///< Детальный учёт cash
    std::vector<PendingOrderInfo> pending_orders;           ///< Активные ордера
    int pending_buy_count{0};                               ///< Количество активных BUY-ордеров
    int pending_sell_count{0};                              ///< Количество активных SELL-ордеров
    double total_fees_today{0.0};                           ///< Суммарные комиссии за сегодня
    double capital_utilization_pct{0.0};                    ///< Использование капитала (%)
};

} // namespace tb::portfolio
