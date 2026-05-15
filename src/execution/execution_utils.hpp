#pragma once
/**
 * @file execution_utils.hpp
 * @brief Общие утилиты модуля исполнения (USDT-M Futures)
 */

#include "execution/order_types.hpp"

namespace tb::execution {

/// Определяет, требуется ли резервирование маржи для данного ордера.
/// Для фьючерсов: открытие позиции (как Long, так и Short) требует маржи.
/// Закрытие позиции не требует дополнительного резервирования.
inline bool requires_margin_reserve(const OrderRecord& order) {
    return order.trade_side != TradeSide::Close;
}

} // namespace tb::execution
