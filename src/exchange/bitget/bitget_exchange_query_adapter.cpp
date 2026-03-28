/// @file bitget_exchange_query_adapter.cpp
/// @brief Реализация адаптера BitgetRestClient → IExchangeQueryService
///
/// Все эндпоинты используют Bitget REST API v2 (spot).
/// Коды ответа Bitget: "00000" = успех, остальные = ошибка.

#include "exchange/bitget/bitget_exchange_query_adapter.hpp"
#include "common/errors.hpp"
#include <boost/json.hpp>
#include <stdexcept>
#include <charconv>

namespace tb::exchange::bitget {

// ============================================================
// Конструктор
// ============================================================

BitgetExchangeQueryAdapter::BitgetExchangeQueryAdapter(
    std::shared_ptr<BitgetRestClient> rest_client,
    std::shared_ptr<logging::ILogger> logger)
    : rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
{}

// ============================================================
// Вспомогательные функции
// ============================================================

std::string BitgetExchangeQueryAdapter::json_str(
    const boost::json::object& obj, std::string_view key)
{
    auto it = obj.find(key);
    if (it == obj.end()) return {};
    if (it->value().is_string()) {
        return std::string(it->value().get_string());
    }
    return {};
}

double BitgetExchangeQueryAdapter::json_dbl(
    const boost::json::object& obj, std::string_view key)
{
    auto it = obj.find(key);
    if (it == obj.end()) return 0.0;
    if (it->value().is_double()) return it->value().get_double();
    if (it->value().is_int64())  return static_cast<double>(it->value().get_int64());
    if (it->value().is_string()) {
        const auto s = it->value().get_string();
        double val = 0.0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
        if (ec == std::errc{}) return val;
    }
    return 0.0;
}

// ============================================================
// Разбор ответов API
// ============================================================

reconciliation::ExchangeOrderInfo BitgetExchangeQueryAdapter::parse_order(
    const boost::json::object& obj)
{
    reconciliation::ExchangeOrderInfo info;
    info.order_id        = OrderId(json_str(obj, "orderId"));
    info.client_order_id = OrderId(json_str(obj, "clientOid"));
    info.symbol          = Symbol(json_str(obj, "symbol"));
    info.price           = Price(json_dbl(obj, "price"));
    info.original_quantity  = Quantity(json_dbl(obj, "size"));
    info.filled_quantity    = Quantity(json_dbl(obj, "baseVolume"));
    info.status             = json_str(obj, "status");

    const auto side_str = json_str(obj, "side");
    info.side = (side_str == "sell") ? Side::Sell : Side::Buy;

    const auto order_type_str = json_str(obj, "orderType");
    if (order_type_str == "market")   info.order_type = OrderType::Market;
    else if (order_type_str == "limit") info.order_type = OrderType::Limit;
    else                               info.order_type = OrderType::Limit;

    const auto ctime_str = json_str(obj, "cTime");
    if (!ctime_str.empty()) {
        int64_t ts_ms = 0;
        auto [ptr, ec] = std::from_chars(ctime_str.data(),
                                         ctime_str.data() + ctime_str.size(), ts_ms);
        if (ec == std::errc{}) {
            info.created_at = Timestamp(ts_ms * 1'000'000LL);
        }
    }

    return info;
}

reconciliation::ExchangePositionInfo BitgetExchangeQueryAdapter::parse_position(
    const boost::json::object& obj)
{
    reconciliation::ExchangePositionInfo info;
    info.symbol    = Symbol(json_str(obj, "coin"));
    info.available = Quantity(json_dbl(obj, "available"));
    info.frozen    = Quantity(json_dbl(obj, "frozen"));
    // Bitget не даёт прямого USD-оценки в этом эндпоинте — оставляем 0
    info.total_value_usd = 0.0;
    return info;
}

// ============================================================
// get_open_orders
// ============================================================

Result<std::vector<reconciliation::ExchangeOrderInfo>>
BitgetExchangeQueryAdapter::get_open_orders(const Symbol& symbol)
{
    std::string query;
    if (!symbol.get().empty()) {
        query = "symbol=" + symbol.get();
    }

    auto resp = rest_client_->get("/api/v2/spot/trade/unfilled-orders", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса unfilled-orders",
                {{"error", resp.error_message},
                 {"status", std::to_string(resp.status_code)}});
        }
        return Err<std::vector<reconciliation::ExchangeOrderInfo>>(
            TbError::ExchangeConnectionFailed);
    }

    std::vector<reconciliation::ExchangeOrderInfo> result;
    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            const auto msg = json_str(root, "msg");
            if (logger_) {
                logger_->warn("reconciliation", "Bitget API вернул ошибку (unfilled-orders)",
                    {{"code", code}, {"msg", msg}});
            }
            return Err<std::vector<reconciliation::ExchangeOrderInfo>>(
                TbError::ExchangeConnectionFailed);
        }

        const auto& data = root.at("data").as_array();
        result.reserve(data.size());
        for (const auto& item : data) {
            result.push_back(parse_order(item.as_object()));
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга ответа unfilled-orders",
                {{"exception", ex.what()}});
        }
        return Err<std::vector<reconciliation::ExchangeOrderInfo>>(
            TbError::ReconciliationFailed);
    }

    return result;
}

// ============================================================
// get_account_balances
// ============================================================

Result<std::vector<reconciliation::ExchangePositionInfo>>
BitgetExchangeQueryAdapter::get_account_balances()
{
    auto resp = rest_client_->get("/api/v2/spot/account/assets", "");
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса account/assets",
                {{"error", resp.error_message}});
        }
        return Err<std::vector<reconciliation::ExchangePositionInfo>>(
            TbError::ExchangeConnectionFailed);
    }

    std::vector<reconciliation::ExchangePositionInfo> result;
    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            if (logger_) {
                logger_->warn("reconciliation", "Bitget API вернул ошибку (assets)",
                    {{"code", code}, {"msg", json_str(root, "msg")}});
            }
            return Err<std::vector<reconciliation::ExchangePositionInfo>>(
                TbError::ExchangeConnectionFailed);
        }

        const auto& data = root.at("data").as_array();
        result.reserve(data.size());
        for (const auto& item : data) {
            result.push_back(parse_position(item.as_object()));
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга ответа account/assets",
                {{"exception", ex.what()}});
        }
        return Err<std::vector<reconciliation::ExchangePositionInfo>>(
            TbError::ReconciliationFailed);
    }

    return result;
}

// ============================================================
// get_order_status
// ============================================================

Result<reconciliation::ExchangeOrderInfo>
BitgetExchangeQueryAdapter::get_order_status(
    const OrderId& order_id, const Symbol& symbol)
{
    const std::string query = "orderId=" + order_id.get()
                            + "&symbol=" + symbol.get();

    auto resp = rest_client_->get("/api/v2/spot/trade/orderInfo", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса orderInfo",
                {{"order_id", order_id.get()}, {"error", resp.error_message}});
        }
        return Err<reconciliation::ExchangeOrderInfo>(TbError::ExchangeConnectionFailed);
    }

    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            if (logger_) {
                logger_->warn("reconciliation", "Bitget API вернул ошибку (orderInfo)",
                    {{"code", code}, {"order_id", order_id.get()}});
            }
            return Err<reconciliation::ExchangeOrderInfo>(TbError::ExchangeConnectionFailed);
        }

        // data — объект (не массив) для одного ордера
        const auto& data = root.at("data").as_object();
        return parse_order(data);

    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга ответа orderInfo",
                {{"exception", ex.what()}, {"order_id", order_id.get()}});
        }
        return Err<reconciliation::ExchangeOrderInfo>(TbError::ReconciliationFailed);
    }
}

} // namespace tb::exchange::bitget
