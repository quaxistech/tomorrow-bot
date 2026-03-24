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

/// Запись об ордере
struct OrderRecord {
    OrderId order_id{OrderId("")};
    OrderId exchange_order_id{OrderId("")};   ///< ID ордера на бирже
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
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
};

/// Преобразование состояния ордера в строку
std::string to_string(OrderState state);

/// Проверка допустимости перехода FSM
bool is_valid_transition(OrderState from, OrderState to);

} // namespace tb::execution
