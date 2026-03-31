#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::execution {

/// Расширенный статус ордера (FSM-состояния)
enum class OrderState {
    New,                    ///< Создан, не отправлен
    PendingAck,             ///< Отправлен, ожидаем подтверждение
    Open,                   ///< Активен в стакане
    PartiallyFilled,        ///< Частично исполнен
    Filled,                 ///< Полностью исполнен
    CancelPending,          ///< Запрос отмены отправлен
    Cancelled,              ///< Отменён
    Rejected,               ///< Отклонён биржей
    Expired,                ///< Истёк
    UnknownRecovery         ///< Неизвестное состояние (восстановление)
};

/// Политика обработки частичного исполнения
enum class PartialFillPolicy {
    WaitForFull,        ///< Ждать полного исполнения
    CancelRemaining,    ///< Отменить остаток после первого fill
    AllowPartial        ///< Принять частичное исполнение как есть
};

/// Запись об отдельном fill event (partial или full)
struct FillEvent {
    OrderId order_id{OrderId("")};
    Quantity fill_quantity{Quantity(0.0)};       ///< Объём этого fill
    Price fill_price{Price(0.0)};               ///< Цена этого fill
    Quantity cumulative_filled{Quantity(0.0)};   ///< Накопленный объём после этого fill
    double fee{0.0};                             ///< Комиссия за этот fill
    std::string trade_id;                        ///< ID сделки на бирже
    Timestamp occurred_at{Timestamp(0)};
};

/// Расширенная информация об исполнении ордера
struct OrderExecutionInfo {
    std::vector<FillEvent> fills;                  ///< Все fill events
    PartialFillPolicy fill_policy{PartialFillPolicy::WaitForFull};
    std::string client_order_id;                   ///< Идемпотентный ключ
    int retry_count{0};                            ///< Кол-во попыток отправки
    int64_t time_in_open_ms{0};                    ///< Время в состоянии Open
    int64_t first_fill_latency_ms{0};              ///< Задержка до первого fill
    Price expected_fill_price{Price(0.0)};         ///< Ожидаемая цена (от execution alpha)
    double realized_slippage{0.0};                 ///< Реальное проскальзывание
};

/// Запись об ордере
struct OrderRecord {
    OrderId order_id{OrderId("")};
    OrderId exchange_order_id{OrderId("")};   ///< ID ордера на бирже
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};  ///< Сторона позиции (Long/Short) для фьючерсов
    TradeSide trade_side{TradeSide::Open};            ///< Открытие/закрытие (для фьючерсов)
    OrderType order_type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GoodTillCancel};
    Price price{Price(0.0)};
    Quantity original_quantity{Quantity(0.0)};
    Quantity filled_quantity{Quantity(0.0)};
    Quantity remaining_quantity{Quantity(0.0)};
    Price avg_fill_price{Price(0.0)};
    OrderState state{OrderState::New};

    StrategyId strategy_id{StrategyId("")};
    CorrelationId correlation_id{CorrelationId("")};

    Timestamp created_at{Timestamp(0)};
    Timestamp last_updated{Timestamp(0)};

    std::string rejection_reason;
    int retry_count{0};
    OrderExecutionInfo execution_info;  ///< Расширенная информация об исполнении

    /// Проверка, является ли состояние терминальным
    bool is_terminal() const {
        return state == OrderState::Filled || state == OrderState::Cancelled ||
               state == OrderState::Rejected || state == OrderState::Expired;
    }

    /// Проверка, является ли ордер активным
    bool is_active() const {
        return state == OrderState::Open || state == OrderState::PartiallyFilled ||
               state == OrderState::PendingAck || state == OrderState::CancelPending;
    }
};

/// Событие FSM-перехода
struct OrderTransition {
    OrderState from;
    OrderState to;
    std::string reason;
    Timestamp occurred_at{Timestamp(0)};
};

/// Результат отправки ордера
struct OrderSubmitResult {
    bool success{false};
    OrderId order_id{OrderId("")};
    OrderId exchange_order_id{OrderId("")};
    std::string error_message;
    Quantity submitted_quantity{Quantity(0.0)};  ///< Фактическое кол-во после floor (отправлено на биржу)
};

/// Преобразование состояния ордера в строку
std::string to_string(OrderState state);

/// Преобразование политики частичного исполнения в строку
std::string to_string(PartialFillPolicy policy);

/// Проверка допустимости перехода FSM
bool is_valid_transition(OrderState from, OrderState to);

} // namespace tb::execution
