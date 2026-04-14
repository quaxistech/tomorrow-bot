/**
 * @file reconciliation_types.hpp
 * @brief Типы данных для модуля reconciliation
 *
 * Определяет структуры для сравнения внутреннего состояния системы
 * с данными биржи: расхождения ордеров, позиций и балансов.
 */
#pragma once

#include "common/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace tb::reconciliation {

// ============================================================
// Перечисления
// ============================================================

/// Состояние reconciliation
enum class ReconciliationStatus {
    Success,            ///< Всё совпадает
    PartialMismatch,    ///< Некритичные расхождения
    CriticalMismatch,   ///< Критическое расхождение (требуется вмешательство)
    Failed              ///< Reconciliation не завершилась (ошибка связи и т.д.)
};

/// Тип расхождения
enum class MismatchType {
    OrderExistsOnlyOnExchange,     ///< Ордер есть на бирже, но нет в системе
    OrderExistsOnlyLocally,        ///< Ордер есть в системе, но нет на бирже
    StateMismatch,                 ///< Состояние расходится (e.g. биржа=Filled, система=Open)
    QuantityMismatch,              ///< filled_qty расходится
    PositionExistsOnlyOnExchange,  ///< Позиция есть на бирже, нет в системе
    PositionExistsOnlyLocally,     ///< Позиция в системе, нет на бирже
    BalanceMismatch                ///< Баланс расходится
};

/// Действие по исправлению
enum class ResolutionAction {
    SyncFromExchange,    ///< Принять данные биржи как истину
    CancelOnExchange,    ///< Отменить ордер на бирже
    CloseOnExchange,     ///< Закрыть позицию на бирже
    UpdateLocalState,    ///< Обновить внутреннее состояние
    AlertOperator,       ///< Уведомить оператора
    NoAction             ///< Нет действия
};

// ============================================================
// to_string для перечислений
// ============================================================

[[nodiscard]] inline const char* to_string(ReconciliationStatus s) {
    switch (s) {
        case ReconciliationStatus::Success:          return "Success";
        case ReconciliationStatus::PartialMismatch:  return "PartialMismatch";
        case ReconciliationStatus::CriticalMismatch: return "CriticalMismatch";
        case ReconciliationStatus::Failed:           return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* to_string(MismatchType t) {
    switch (t) {
        case MismatchType::OrderExistsOnlyOnExchange:    return "OrderExistsOnlyOnExchange";
        case MismatchType::OrderExistsOnlyLocally:       return "OrderExistsOnlyLocally";
        case MismatchType::StateMismatch:                return "StateMismatch";
        case MismatchType::QuantityMismatch:             return "QuantityMismatch";
        case MismatchType::PositionExistsOnlyOnExchange: return "PositionExistsOnlyOnExchange";
        case MismatchType::PositionExistsOnlyLocally:    return "PositionExistsOnlyLocally";
        case MismatchType::BalanceMismatch:              return "BalanceMismatch";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* to_string(ResolutionAction a) {
    switch (a) {
        case ResolutionAction::SyncFromExchange:  return "SyncFromExchange";
        case ResolutionAction::CancelOnExchange:  return "CancelOnExchange";
        case ResolutionAction::CloseOnExchange:   return "CloseOnExchange";
        case ResolutionAction::UpdateLocalState:  return "UpdateLocalState";
        case ResolutionAction::AlertOperator:     return "AlertOperator";
        case ResolutionAction::NoAction:          return "NoAction";
    }
    return "Unknown";
}

// ============================================================
// Структуры данных
// ============================================================

/// Запись о расхождении
struct MismatchRecord {
    MismatchType type;
    Symbol symbol{Symbol("")};
    OrderId order_id{OrderId("")};    ///< Может быть пустым для позиций/балансов
    std::string description;
    ResolutionAction resolved_by{ResolutionAction::NoAction};
    bool resolved{false};
    Timestamp detected_at{Timestamp(0)};
    /// A2 fix: сторона позиции для leg-aware resolution в hedge mode
    std::optional<PositionSide> position_side;
};

/// Результат reconciliation
struct ReconciliationResult {
    ReconciliationStatus status{ReconciliationStatus::Success};
    std::vector<MismatchRecord> mismatches;
    int orders_reconciled{0};
    int positions_reconciled{0};
    int balances_checked{0};
    int auto_resolved{0};
    int operator_escalated{0};
    int64_t duration_ms{0};
    Timestamp completed_at{Timestamp(0)};
};

/// Данные ордера с биржи (упрощённая структура для reconciliation)
struct ExchangeOrderInfo {
    OrderId order_id{OrderId("")};
    OrderId client_order_id{OrderId("")};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};  ///< Направление позиции (фьючерсы hedge mode)
    TradeSide trade_side{TradeSide::Open};            ///< Открытие/закрытие (фьючерсы)
    OrderType order_type{OrderType::Limit};
    Price price{Price(0.0)};
    Quantity original_quantity{Quantity(0.0)};
    Quantity filled_quantity{Quantity(0.0)};
    std::string status;       ///< "live", "filled", "cancelled" и т.д.
    Timestamp created_at{Timestamp(0)};
};

/// Маржинальный баланс аккаунта с биржи (USDT-M Futures).
/// Используется для сверки USDT cash balance; не для фьючерсных позиций.
struct ExchangePositionInfo {
    Symbol symbol{Symbol("")};           ///< Маржинальный актив (e.g. "USDT")
    Quantity available{Quantity(0.0)};    ///< Доступный баланс
    Quantity frozen{Quantity(0.0)};       ///< Заблокированный (маржа + ордера)
    double total_value_usd{0.0};          ///< Эквивалент в USD
};

/// Открытая фьючерсная позиция с биржи (USDT-M).
/// Используется для reconciliation и startup recovery.
struct ExchangeOpenPositionInfo {
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};                                ///< Buy = Long direction, Sell = Short
    PositionSide position_side{PositionSide::Long};      ///< Hedge mode: Long/Short
    Quantity size{Quantity(0.0)};                         ///< Размер позиции (контрактов)
    Price entry_price{Price(0.0)};                       ///< Средняя цена входа
    Price current_price{Price(0.0)};                     ///< Текущая mark price
    double notional_usd{0.0};                            ///< mark_price × size
    double unrealized_pnl{0.0};                          ///< Нереализованная P&L (USDT)
};

/// Конфигурация reconciliation (USDT-M Futures)
struct ReconciliationConfig {
    bool auto_resolve_state_mismatches{true};
    bool auto_cancel_orphan_orders{false};       ///< Безопаснее: false по умолчанию
    bool auto_close_orphan_positions{false};      ///< Безопаснее: false по умолчанию
    /// Допустимое расхождение размера фьючерсной позиции (%).
    /// 0.5% — институциональный стандарт reconciliation
    /// (Yurko & Greenhalgh, "Position Reconciliation in Algorithmic Trading", 2022).
    double position_tolerance_pct{0.5};
    /// Допустимое расхождение маржинального баланса USDT (%).
    /// 1.0% — учитывает unrealized PnL drift между тиками,
    /// funding fee charges (каждые 8ч) и rounding комиссий.
    double balance_tolerance_pct{1.0};
    int max_auto_resolutions_per_run{10};
};

} // namespace tb::reconciliation
