#pragma once
/**
 * @file bitget_order_submitter.hpp
 * @brief Реализация IOrderSubmitter для Bitget REST API v2 (Spot)
 *
 * Отправляет реальные ордера на биржу Bitget через REST API.
 * Поддерживает Market, Limit и PostOnly ордера.
 */

#include "bitget_rest_client.hpp"
#include "execution/order_submitter.hpp"
#include "common/exchange_rules.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    bool cancel_order(const OrderId& order_id, const Symbol& symbol) override;
    Price query_order_fill_price(const OrderId& exchange_order_id, const Symbol& symbol) override;

    /// Установить правила инструмента (precision, min notional, min qty)
    void set_rules(const Symbol& symbol, const ExchangeSymbolRules& rules);

    /// Получить правила для конкретного символа (для диагностики)
    [[nodiscard]] const ExchangeSymbolRules& rules(const Symbol& symbol) const;

private:
    /// Построить JSON тело запроса для размещения ордера
    std::string build_place_order_json(const execution::OrderRecord& order) const;

    /// Получить правила символа или fallback
    [[nodiscard]] const ExchangeSymbolRules& get_rules(const Symbol& symbol) const;

    std::shared_ptr<BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    std::string product_type_;

    /// Правила инструментов по символам (из exchange info)
    std::unordered_map<std::string, ExchangeSymbolRules> rules_by_symbol_;
    ExchangeSymbolRules default_rules_;   ///< Fallback при отсутствии символа
    mutable std::mutex rules_mutex_;
};

} // namespace tb::exchange::bitget
