#pragma once
/**
 * @file bitget_futures_order_submitter.hpp
 * @brief Реализация IOrderSubmitter для Bitget Mix v2 API (USDT-M Futures)
 *
 * Отправляет фьючерсные ордера через Bitget Mix REST API v2.
 * Ключевые отличия от Spot:
 *  - tradeSide: "open" / "close" — критично для открытия/закрытия
 *  - productType: "USDT-FUTURES"
 *  - marginCoin: "USDT"
 *  - size всегда в base currency (не quote!)
 *  - Дополнительные методы: set_leverage, set_margin_mode
 */

#include "bitget_rest_client.hpp"
#include "execution/order_submitter.hpp"
#include "common/exchange_rules.hpp"
#include "config/config_types.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tb::exchange::bitget {

/**
 * @brief Продакшн-реализация отправки ордеров через Bitget Mix v2 API (Futures)
 *
 * Маршруты:
 *  - submit:       POST /api/v2/mix/order/place-order
 *  - cancel:       POST /api/v2/mix/order/cancel-order
 *  - order info:   GET  /api/v2/mix/order/detail
 *  - leverage:     POST /api/v2/mix/account/set-leverage
 *  - margin mode:  POST /api/v2/mix/account/set-margin-mode
 */
class BitgetFuturesOrderSubmitter : public execution::IOrderSubmitter {
public:
    BitgetFuturesOrderSubmitter(
        std::shared_ptr<BitgetRestClient> rest_client,
        std::shared_ptr<logging::ILogger> logger,
        config::FuturesConfig futures_config);

    execution::OrderSubmitResult submit_order(const execution::OrderRecord& order) override;
    bool cancel_order(const OrderId& order_id, const Symbol& symbol) override;
    Price query_order_fill_price(const OrderId& exchange_order_id, const Symbol& symbol) override;
    execution::OrderFillDetail query_order_fill_detail(const OrderId& exchange_order_id, const Symbol& symbol) override;

    /// Установить правила инструмента (precision, min notional, min qty)
    void set_rules(const Symbol& symbol, const ExchangeSymbolRules& rules);

    /// Получить правила для конкретного символа (для диагностики)
    [[nodiscard]] const ExchangeSymbolRules& rules(const Symbol& symbol) const;

    /// Установить кредитное плечо для символа на бирже
    /// @param symbol Торговый символ (e.g. "BTCUSDT")
    /// @param leverage Значение плеча [1, 125]
    /// @param hold_side "long" / "short" (для isolated margin оба нужны)
    /// @return true если успешно
    bool set_leverage(const Symbol& symbol, int leverage, const std::string& hold_side);

    /// Установить режим маржи для символа (isolated / crossed)
    /// @return true если успешно
    bool set_margin_mode(const Symbol& symbol, const std::string& margin_mode);

    /// Установить hold mode (single_hold / double_hold)
    /// "double_hold" = hedge mode (long + short одновременно)
    /// "single_hold" = one-way mode
    bool set_hold_mode(const std::string& product_type, const std::string& hold_mode);

private:
    /// Построить JSON тело запроса для размещения фьючерсного ордера
    std::string build_place_order_json(const execution::OrderRecord& order) const;

    /// Получить правила символа или fallback
    [[nodiscard]] const ExchangeSymbolRules& get_rules(const Symbol& symbol) const;

    std::shared_ptr<BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    config::FuturesConfig futures_config_;

    /// Правила инструментов по символам (из exchange info)
    std::unordered_map<std::string, ExchangeSymbolRules> rules_by_symbol_;
    ExchangeSymbolRules default_rules_;   ///< Fallback при отсутствии символа
    mutable std::mutex rules_mutex_;

    /// Кеш последнего успешно установленного leverage: "SYMBOL:long" -> leverage
    std::unordered_map<std::string, int> leverage_cache_;
    /// Кеш максимального leverage для символа (обнаруженный через 40797)
    std::unordered_map<std::string, int> max_leverage_cache_;
    mutable std::mutex leverage_cache_mutex_;
};

} // namespace tb::exchange::bitget
