#pragma once
/**
 * @file bitget_order_submitter.hpp
 * @brief Реализация IOrderSubmitter для Bitget REST API v2 (Spot)
 *
 * Отправляет реальные ордера на биржу Bitget через REST API.
 * Поддерживает Market, Limit и PostOnly ордера.
 */

#include "bitget_rest_client.hpp"
#include "execution/execution_engine.hpp"
#include <memory>
#include <string>

namespace tb::exchange::bitget {

/**
 * @brief Продакшн-реализация отправки ордеров через Bitget Spot v2 API
 *
 * Маршруты:
 *  - submit:  POST /api/v2/spot/trade/place-order
 *  - cancel:  POST /api/v2/spot/trade/cancel-order
 */
class BitgetOrderSubmitter : public execution::IOrderSubmitter {
public:
    BitgetOrderSubmitter(
        std::shared_ptr<BitgetRestClient> rest_client,
        std::shared_ptr<logging::ILogger> logger,
        std::string product_type = "SPOT"
    );

    execution::OrderSubmitResult submit_order(const execution::OrderRecord& order) override;
    bool cancel_order(const OrderId& order_id) override;

    /// Установить точность для конкретного символа (из exchange info)
    void set_symbol_precision(int quantity_scale, int price_scale) {
        quantity_scale_ = quantity_scale;
        price_scale_ = price_scale;
    }

private:
    /// Построить JSON тело запроса для размещения ордера
    std::string build_place_order_json(const execution::OrderRecord& order) const;

    std::shared_ptr<BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    std::string product_type_;

    /// Точность ордера для текущего символа (из exchange info)
    int quantity_scale_{6};   ///< Кол-во знаков количества (по умолчанию 6 для BTC)
    int price_scale_{2};      ///< Кол-во знаков цены (по умолчанию 2 для USDT пар)

    /// Символ последнего отправленного ордера (для cancel_order, которому нужен symbol)
    mutable std::string last_order_symbol_{"BTCUSDT"};
};

} // namespace tb::exchange::bitget
