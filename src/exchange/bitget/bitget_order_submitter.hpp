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

private:
    /// Построить JSON тело запроса для размещения ордера
    std::string build_place_order_json(const execution::OrderRecord& order) const;

    std::shared_ptr<BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    std::string product_type_;
};

} // namespace tb::exchange::bitget
