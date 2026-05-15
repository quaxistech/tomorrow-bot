#pragma once
/**
 * @file order_submitter.hpp
 * @brief IOrderSubmitter — интерфейс взаимодействия с биржей (§15 ТЗ)
 *
 * Вынесен в отдельный файл для разрыва циклической зависимости.
 */

#include "common/exchange_rules.hpp"
#include "execution/order_types.hpp"
#include <memory>

namespace tb::execution {

/// Интерфейс отправки ордеров на биржу (абстракция для тестирования)
class IOrderSubmitter {
public:
    virtual ~IOrderSubmitter() = default;

    /// Отправить ордер
    virtual OrderSubmitResult submit_order(const OrderRecord& order) = 0;

    /// Отменить ордер. Symbol обязателен для Bitget API.
    virtual bool cancel_order(const OrderId& order_id, const Symbol& symbol) = 0;

    /// Запросить реальную цену исполнения market-ордера с биржи.
    /// Возвращает avg fill price > 0 при успехе, 0.0 при неудаче.
    virtual Price query_order_fill_price(const OrderId& exchange_order_id,
                                          const Symbol& symbol) {
        (void)exchange_order_id; (void)symbol;
        return Price(0.0);
    }

    /// Запросить полные детали исполнения ордера с биржи (цена + количество + статус).
    /// Используется для подтверждения market fill'ов вместо оптимистичного предположения.
    virtual OrderFillDetail query_order_fill_detail(const OrderId& exchange_order_id,
                                                     const Symbol& symbol) {
        (void)exchange_order_id; (void)symbol;
        return OrderFillDetail{};
    }
};

} // namespace tb::execution
