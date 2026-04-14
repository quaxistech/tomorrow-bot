/// @file bitget_futures_query_adapter.cpp
/// @brief Реализация адаптера BitgetRestClient → IExchangeQueryService (USDT-M Futures)
///
/// Все эндпоинты используют Bitget Mix REST API v2.

#include "exchange/bitget/bitget_futures_query_adapter.hpp"
#include "common/errors.hpp"
#include <boost/json.hpp>
#include <stdexcept>
#include <charconv>

namespace tb::exchange::bitget {

// ============================================================
// Конструктор
// ============================================================

BitgetFuturesQueryAdapter::BitgetFuturesQueryAdapter(
    std::shared_ptr<BitgetRestClient> rest_client,
    std::shared_ptr<logging::ILogger> logger,
    config::FuturesConfig futures_config)
    : rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , futures_config_(std::move(futures_config))
{}

// ============================================================
// Вспомогательные функции
// ============================================================

std::string BitgetFuturesQueryAdapter::json_str(
    const boost::json::object& obj, std::string_view key)
{
    auto it = obj.find(key);
    if (it == obj.end()) return {};
    if (it->value().is_string()) {
        return std::string(it->value().get_string());
    }
    return {};
}

double BitgetFuturesQueryAdapter::json_dbl(
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

reconciliation::ExchangeOrderInfo BitgetFuturesQueryAdapter::parse_order(
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

    // Side для фьючерсов: "buy" / "sell"
    const auto side_str = json_str(obj, "side");
    info.side = (side_str == "sell") ? Side::Sell : Side::Buy;

    // Position side (hedge mode): "long" / "short"
    const auto hold_side_str = json_str(obj, "posSide");
    info.position_side = (hold_side_str == "short") ? PositionSide::Short : PositionSide::Long;

    // Trade side: "open" / "close"
    const auto trade_side_str = json_str(obj, "tradeSide");
    info.trade_side = (trade_side_str == "close") ? TradeSide::Close : TradeSide::Open;

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

FuturesPositionInfo BitgetFuturesQueryAdapter::parse_futures_position(
    const boost::json::object& obj)
{
    FuturesPositionInfo info;
    info.symbol           = Symbol(json_str(obj, "symbol"));
    info.total            = Quantity(json_dbl(obj, "total"));
    info.available        = Quantity(json_dbl(obj, "available"));
    info.entry_price      = Price(json_dbl(obj, "openPriceAvg"));
    info.liquidation_price = Price(json_dbl(obj, "liquidationPrice"));
    info.mark_price       = Price(json_dbl(obj, "markPrice"));
    info.unrealized_pnl   = json_dbl(obj, "unrealizedPL");
    info.margin           = json_dbl(obj, "marginSize");
    info.leverage         = static_cast<int>(json_dbl(obj, "leverage"));
    info.margin_mode      = json_str(obj, "marginMode");

    const auto hold_side = json_str(obj, "holdSide");
    info.position_side = (hold_side == "short") ? PositionSide::Short : PositionSide::Long;

    return info;
}

// ============================================================
// get_open_orders (фьючерсы)
// ============================================================

Result<std::vector<reconciliation::ExchangeOrderInfo>>
BitgetFuturesQueryAdapter::get_open_orders(const Symbol& symbol)
{
    std::string query = "productType=" + futures_config_.product_type;
    if (!symbol.get().empty()) {
        query += "&symbol=" + symbol.get();
    }

    auto resp = rest_client_->get("/api/v2/mix/order/orders-pending", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса futures orders-pending",
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
                logger_->warn("reconciliation", "Bitget Futures API ошибка (orders-pending)",
                    {{"code", code}, {"msg", msg}});
            }
            return Err<std::vector<reconciliation::ExchangeOrderInfo>>(
                TbError::ExchangeConnectionFailed);
        }

        // Mix API v2: data.entrustedList — массив ордеров
        const auto& data = root.at("data").as_object();
        const auto orders_it = data.find("entrustedList");
        if (orders_it != data.end() && orders_it->value().is_array()) {
            const auto& orders = orders_it->value().as_array();
            result.reserve(orders.size());
            for (const auto& item : orders) {
                result.push_back(parse_order(item.as_object()));
            }
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга ответа futures orders-pending",
                {{"exception", ex.what()}});
        }
        return Err<std::vector<reconciliation::ExchangeOrderInfo>>(
            TbError::ReconciliationFailed);
    }

    return result;
}

// ============================================================
// get_account_balances (фьючерсный маржинальный аккаунт)
// ============================================================

Result<std::vector<reconciliation::ExchangePositionInfo>>
BitgetFuturesQueryAdapter::get_account_balances()
{
    std::string query = "productType=" + futures_config_.product_type;

    auto resp = rest_client_->get("/api/v2/mix/account/accounts", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса futures accounts",
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
                logger_->warn("reconciliation", "Bitget Futures API ошибка (accounts)",
                    {{"code", code}, {"msg", json_str(root, "msg")}});
            }
            return Err<std::vector<reconciliation::ExchangePositionInfo>>(
                TbError::ExchangeConnectionFailed);
        }

        const auto& data = root.at("data").as_array();
        result.reserve(data.size());
        for (const auto& item : data) {
            const auto& acc = item.as_object();
            reconciliation::ExchangePositionInfo info;
            info.symbol    = Symbol(json_str(acc, "marginCoin"));
            info.available = Quantity(json_dbl(acc, "available"));
            info.frozen    = Quantity(json_dbl(acc, "locked"));
            info.total_value_usd = json_dbl(acc, "usdtEquity");
            result.push_back(std::move(info));
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга ответа futures accounts",
                {{"exception", ex.what()}});
        }
        return Err<std::vector<reconciliation::ExchangePositionInfo>>(
            TbError::ReconciliationFailed);
    }

    return result;
}

Result<std::vector<reconciliation::ExchangeOpenPositionInfo>>
BitgetFuturesQueryAdapter::get_open_positions(const Symbol& symbol)
{
    Result<std::vector<FuturesPositionInfo>> positions_result = symbol.get().empty()
        ? get_all_positions()
        : get_positions(symbol);

    if (!positions_result.has_value()) {
        return Err<std::vector<reconciliation::ExchangeOpenPositionInfo>>(positions_result.error());
    }

    std::vector<reconciliation::ExchangeOpenPositionInfo> result;
    result.reserve(positions_result->size());

    for (const auto& pos : positions_result.value()) {
        reconciliation::ExchangeOpenPositionInfo info;
        info.symbol = pos.symbol;
        info.side = (pos.position_side == PositionSide::Short) ? Side::Sell : Side::Buy;
        info.position_side = pos.position_side;
        info.size = pos.total;
        info.entry_price = pos.entry_price;
        info.current_price = pos.mark_price;
        info.notional_usd = pos.mark_price.get() * pos.total.get();
        info.unrealized_pnl = pos.unrealized_pnl;
        result.push_back(std::move(info));
    }

    return result;
}

// ============================================================
// get_order_status (фьючерсы)
// ============================================================

Result<reconciliation::ExchangeOrderInfo>
BitgetFuturesQueryAdapter::get_order_status(
    const OrderId& order_id, const Symbol& symbol)
{
    const std::string query = "symbol=" + symbol.get()
                            + "&productType=" + futures_config_.product_type
                            + "&orderId=" + order_id.get();

    auto resp = rest_client_->get("/api/v2/mix/order/detail", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка запроса futures order detail",
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
                logger_->warn("reconciliation", "Bitget Futures API ошибка (order detail)",
                    {{"code", code}, {"order_id", order_id.get()}});
            }
            return Err<reconciliation::ExchangeOrderInfo>(TbError::ExchangeConnectionFailed);
        }

        // Mix API v2 detail: data может быть объектом или массивом
        const auto& data_val = root.at("data");
        if (data_val.is_object()) {
            return parse_order(data_val.as_object());
        } else if (data_val.is_array() && !data_val.as_array().empty()) {
            return parse_order(data_val.as_array()[0].as_object());
        }

        if (logger_) {
            logger_->warn("reconciliation", "Пустой ответ futures order detail",
                {{"order_id", order_id.get()}});
        }
        return Err<reconciliation::ExchangeOrderInfo>(TbError::ReconciliationFailed);

    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("reconciliation", "Ошибка парсинга futures order detail",
                {{"exception", ex.what()}, {"order_id", order_id.get()}});
        }
        return Err<reconciliation::ExchangeOrderInfo>(TbError::ReconciliationFailed);
    }
}

// ============================================================
// get_all_positions (все фьючерсные позиции)
// ============================================================

Result<std::vector<FuturesPositionInfo>>
BitgetFuturesQueryAdapter::get_all_positions()
{
    std::string query = "productType=" + futures_config_.product_type
                      + "&marginCoin=" + futures_config_.margin_coin;

    auto resp = rest_client_->get("/api/v2/mix/position/all-position", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("FuturesQuery", "Ошибка запроса all-position",
                {{"error", resp.error_message}});
        }
        return Err<std::vector<FuturesPositionInfo>>(TbError::ExchangeConnectionFailed);
    }

    std::vector<FuturesPositionInfo> result;
    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            if (logger_) {
                logger_->warn("FuturesQuery", "Bitget Futures API ошибка (all-position)",
                    {{"code", code}, {"msg", json_str(root, "msg")}});
            }
            return Err<std::vector<FuturesPositionInfo>>(TbError::ExchangeConnectionFailed);
        }

        const auto& data = root.at("data").as_array();
        result.reserve(data.size());
        for (const auto& item : data) {
            auto pos = parse_futures_position(item.as_object());
            // Фильтруем пустые позиции (total == 0)
            if (pos.total.get() > 0.0) {
                result.push_back(std::move(pos));
            }
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("FuturesQuery", "Ошибка парсинга all-position",
                {{"exception", ex.what()}});
        }
        return Err<std::vector<FuturesPositionInfo>>(TbError::ReconciliationFailed);
    }

    return result;
}

// ============================================================
// get_positions (позиции по символу)
// ============================================================

Result<std::vector<FuturesPositionInfo>>
BitgetFuturesQueryAdapter::get_positions(const Symbol& symbol)
{
    std::string query = "symbol=" + symbol.get()
                      + "&productType=" + futures_config_.product_type
                      + "&marginCoin=" + futures_config_.margin_coin;

    auto resp = rest_client_->get("/api/v2/mix/position/single-position", query);
    if (!resp.success) {
        if (logger_) {
            logger_->warn("FuturesQuery", "Ошибка запроса single-position",
                {{"error", resp.error_message}, {"symbol", symbol.get()}});
        }
        return Err<std::vector<FuturesPositionInfo>>(TbError::ExchangeConnectionFailed);
    }

    std::vector<FuturesPositionInfo> result;
    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            if (logger_) {
                logger_->warn("FuturesQuery", "Bitget Futures API ошибка (single-position)",
                    {{"code", code}, {"msg", json_str(root, "msg")}});
            }
            return Err<std::vector<FuturesPositionInfo>>(TbError::ExchangeConnectionFailed);
        }

        const auto& data = root.at("data").as_array();
        result.reserve(data.size());
        for (const auto& item : data) {
            auto pos = parse_futures_position(item.as_object());
            if (pos.total.get() > 0.0) {
                result.push_back(std::move(pos));
            }
        }
    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->warn("FuturesQuery", "Ошибка парсинга single-position",
                {{"exception", ex.what()}, {"symbol", symbol.get()}});
        }
        return Err<std::vector<FuturesPositionInfo>>(TbError::ReconciliationFailed);
    }

    return result;
}

// ============================================================
// get_current_funding_rate (текущий funding rate для символа)
// ============================================================

double BitgetFuturesQueryAdapter::get_current_funding_rate(const Symbol& symbol)
{
    const std::string query = "symbol=" + symbol.get()
                            + "&productType=" + futures_config_.product_type;

    auto resp = rest_client_->get("/api/v2/mix/market/current-fund-rate", query);
    if (!resp.success) {
        if (logger_) {
            logger_->debug("FuturesQuery", "Не удалось получить funding rate",
                {{"symbol", symbol.get()}, {"error", resp.error_message}});
        }
        return 0.0;
    }

    try {
        auto doc = boost::json::parse(resp.body);
        const auto& root = doc.as_object();

        const auto code = json_str(root, "code");
        if (code != "00000") {
            if (logger_) {
                logger_->debug("FuturesQuery", "Bitget API ошибка (funding rate)",
                    {{"code", code}, {"msg", json_str(root, "msg")},
                     {"symbol", symbol.get()}});
            }
            return 0.0;
        }

        // Bitget Mix v2: { "data": [{ "symbol": "...", "fundingRate": "0.000125" }] }
        const auto& data_val = root.at("data");
        if (data_val.is_array()) {
            const auto& arr = data_val.as_array();
            if (!arr.empty() && arr[0].is_object()) {
                return json_dbl(arr[0].as_object(), "fundingRate");
            }
            return 0.0;
        }
        if (data_val.is_object()) {
            return json_dbl(data_val.as_object(), "fundingRate");
        }
        return 0.0;

    } catch (const std::exception& ex) {
        if (logger_) {
            logger_->debug("FuturesQuery", "Ошибка парсинга funding rate",
                {{"exception", ex.what()}, {"symbol", symbol.get()}});
        }
        return 0.0;
    }
}

} // namespace tb::exchange::bitget
