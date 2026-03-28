#pragma once

/// @file bitget_exchange_query_adapter.hpp
/// @brief Адаптер BitgetRestClient → IExchangeQueryService для reconciliation
///
/// Реализует интерфейс `reconciliation::IExchangeQueryService` поверх
/// `BitgetRestClient`, преобразуя ответы Bitget REST API v2 в унифицированные
/// типы `ExchangeOrderInfo` и `ExchangePositionInfo`.

#include "reconciliation/reconciliation_engine.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "logging/logger.hpp"
#include <boost/json.hpp>
#include <memory>
#include <vector>
#include <string>

namespace tb::exchange::bitget {

/// Адаптер Bitget REST API → IExchangeQueryService
class BitgetExchangeQueryAdapter : public reconciliation::IExchangeQueryService {
public:
    BitgetExchangeQueryAdapter(
        std::shared_ptr<BitgetRestClient> rest_client,
        std::shared_ptr<logging::ILogger> logger);

    /// Получить открытые ордера (необязательный фильтр по символу)
    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_open_orders(const Symbol& symbol = Symbol("")) override;

    /// Получить балансы всех активов аккаунта
    Result<std::vector<reconciliation::ExchangePositionInfo>>
    get_account_balances() override;

    /// Получить статус конкретного ордера
    Result<reconciliation::ExchangeOrderInfo>
    get_order_status(const OrderId& order_id, const Symbol& symbol) override;

private:
    /// Разобрать JSON-объект ордера Bitget → ExchangeOrderInfo
    [[nodiscard]] static reconciliation::ExchangeOrderInfo
    parse_order(const boost::json::object& obj);

    /// Разобрать JSON-объект баланса Bitget → ExchangePositionInfo
    [[nodiscard]] static reconciliation::ExchangePositionInfo
    parse_position(const boost::json::object& obj);

    /// Безопасно извлечь строковое поле из JSON-объекта (пустая строка при ошибке)
    [[nodiscard]] static std::string json_str(
        const boost::json::object& obj, std::string_view key);

    /// Безопасно извлечь числовое поле (double) из JSON-объекта (0.0 при ошибке)
    [[nodiscard]] static double json_dbl(
        const boost::json::object& obj, std::string_view key);

    std::shared_ptr<BitgetRestClient>  rest_client_;
    std::shared_ptr<logging::ILogger>  logger_;
};

} // namespace tb::exchange::bitget
