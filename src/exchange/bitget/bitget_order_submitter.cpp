/**
 * @file bitget_order_submitter.cpp
 * @brief Реализация отправки ордеров через Bitget Spot REST API v2
 */

#include "bitget_order_submitter.hpp"
#include "common/enums.hpp"

#include <boost/json.hpp>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace tb::exchange::bitget {

static constexpr char kComp[] = "BitgetSubmitter";

// Эндпоинты Bitget Spot v2 API
static constexpr char kPlaceOrderPath[]  = "/api/v2/spot/trade/place-order";
static constexpr char kCancelOrderPath[] = "/api/v2/spot/trade/cancel-order";
static constexpr char kOrderInfoPath[]   = "/api/v2/spot/trade/orderInfo";

BitgetOrderSubmitter::BitgetOrderSubmitter(
    std::shared_ptr<BitgetRestClient> rest_client,
    std::shared_ptr<logging::ILogger> logger,
    std::string product_type)
    : rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , product_type_(std::move(product_type))
{
    logger_->info(kComp, "Bitget Order Submitter создан",
        {{"product_type", product_type_}});
}

// ==================== Управление правилами символов ====================

void BitgetOrderSubmitter::set_rules(const Symbol& symbol, const ExchangeSymbolRules& rules) {
    std::lock_guard lock(rules_mutex_);
    rules_by_symbol_[symbol.get()] = rules;
    logger_->debug(kComp, "Правила символа установлены",
        {{"symbol", symbol.get()},
         {"price_precision", std::to_string(rules.price_precision)},
         {"quantity_precision", std::to_string(rules.quantity_precision)},
         {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
}

const ExchangeSymbolRules& BitgetOrderSubmitter::rules(const Symbol& symbol) const {
    return get_rules(symbol);
}

const ExchangeSymbolRules& BitgetOrderSubmitter::get_rules(const Symbol& symbol) const {
    std::lock_guard lock(rules_mutex_);
    auto it = rules_by_symbol_.find(symbol.get());
    if (it != rules_by_symbol_.end()) {
        return it->second;
    }
    return default_rules_;
}

// ==================== Формирование JSON тела запроса ====================

std::string BitgetOrderSubmitter::build_place_order_json(
    const execution::OrderRecord& order) const
{
    const auto& sym_rules = get_rules(order.symbol);

    boost::json::object obj;

    obj["symbol"] = order.symbol.get();

    // Идемпотентный clientOid для защиты от дублирования при retry
    if (!order.execution_info.client_order_id.empty()) {
        obj["clientOid"] = order.execution_info.client_order_id;
    }

    // Направление: buy / sell
    obj["side"] = std::string(to_string(order.side));

    // Тип ордера и force
    bool is_market = (order.order_type == OrderType::Market);
    bool is_post_only = (order.order_type == OrderType::PostOnly);
    bool is_limit = (order.order_type == OrderType::Limit);

    // Проверка неподдерживаемых типов ордеров
    if (order.order_type == OrderType::StopMarket || order.order_type == OrderType::StopLimit) {
        logger_->error(kComp, "Стоп-ордера не поддерживаются Bitget Spot API v2",
            {{"order_type", std::string(to_string(order.order_type))}});
        return "{}";
    }

    if (is_market) {
        obj["orderType"] = "market";
        // Для market ордеров force не отправляем — Bitget его не принимает
    } else if (is_post_only) {
        obj["orderType"] = "limit";
        obj["force"] = "post_only";
    } else if (is_limit) {
        obj["orderType"] = "limit";

        // TimeInForce → force
        switch (order.tif) {
            case TimeInForce::GoodTillCancel:    obj["force"] = "gtc"; break;
            case TimeInForce::ImmediateOrCancel: obj["force"] = "ioc"; break;
            case TimeInForce::FillOrKill:        obj["force"] = "fok"; break;
            default:                             obj["force"] = "gtc"; break;
        }
    } else {
        logger_->error(kComp, "Неизвестный тип ордера",
            {{"order_type", std::string(to_string(order.order_type))}});
        return "{}";
    }

    // Размер ордера
    // Для market buy — size = сумма в USDT (quote currency)
    // Для market sell — size = количество в BTC (base currency)
    // Для limit — size = количество в BTC (base currency)
    if (is_market && order.side == Side::Buy) {
        // Bitget Spot market buy: size = quote amount (USDT to spend).
        if (order.price.get() <= 0.0) {
            logger_->error(kComp, "Market buy отклонён: цена не задана (нельзя вычислить quote amount)",
                {{"symbol", order.symbol.get()},
                 {"qty", std::to_string(order.original_quantity.get())}});
            return "{}";
        }
        double quote_amount = order.original_quantity.get() * order.price.get();
        if (quote_amount < sym_rules.min_trade_usdt) {
            logger_->error(kComp, "Market buy отклонён: quote amount ниже минимума",
                {{"quote_amount", std::to_string(quote_amount)},
                 {"min_trade_usdt", std::to_string(sym_rules.min_trade_usdt)}});
            return "{}";
        }
        obj["size"] = sym_rules.format_price(quote_amount);
    } else {
        // Limit или market sell: size = base currency quantity.
        double floored_qty = sym_rules.floor_quantity(order.original_quantity.get());
        if (!sym_rules.is_quantity_valid(floored_qty)) {
            logger_->error(kComp, "Ордер отклонён: quantity невалиден после округления",
                {{"original_qty", std::to_string(order.original_quantity.get())},
                 {"floored_qty", std::to_string(floored_qty)},
                 {"symbol", order.symbol.get()}});
            return "{}";
        }
        obj["size"] = sym_rules.format_quantity(floored_qty);
    }

    // Цена — только для лимитных ордеров
    if (!is_market) {
        if (order.price.get() <= 0.0) {
            logger_->error(kComp, "Лимитный ордер без цены",
                {{"symbol", order.symbol.get()}});
            return "{}";
        }
        obj["price"] = sym_rules.format_price(order.price.get());
    }

    return boost::json::serialize(obj);
}

// ==================== submit_order ====================

execution::OrderSubmitResult BitgetOrderSubmitter::submit_order(
    const execution::OrderRecord& order)
{
    execution::OrderSubmitResult result;
    result.order_id = order.order_id;

    try {
        std::string body = build_place_order_json(order);

        // Проверка на ошибку формирования JSON (пустой объект)
        if (body == "{}") {
            result.success = false;
            result.error_message = "Ошибка формирования JSON ордера";
            return result;
        }

        logger_->info(kComp, "Отправка ордера на биржу",
            {{"symbol", order.symbol.get()},
             {"side", std::string(to_string(order.side))},
             {"type", std::string(to_string(order.order_type))},
             {"qty", std::to_string(order.original_quantity.get())},
             {"price", std::to_string(order.price.get())}});

        auto response = rest_client_->post(kPlaceOrderPath, body);

        if (!response.success) {
            result.success = false;
            result.error_message = "HTTP ошибка: " + response.error_message;
            logger_->error(kComp, "Ордер не отправлен (HTTP)",
                {{"error", response.error_message},
                 {"body", response.body.substr(0, 512)}});
            return result;
        }

        // Парсинг JSON-ответа Bitget
        auto json = boost::json::parse(response.body);
        auto& obj = json.as_object();

        std::string code = std::string(obj.at("code").as_string());
        std::string msg = obj.contains("msg")
            ? std::string(obj.at("msg").as_string()) : "";

        if (code == "00000") {
            // Успех
            auto& data = obj.at("data").as_object();
            std::string exchange_id = std::string(data.at("orderId").as_string());

            result.success = true;
            result.exchange_order_id = OrderId(exchange_id);

            logger_->info(kComp, "Ордер принят биржей",
                {{"exchange_order_id", exchange_id},
                 {"internal_order_id", order.order_id.get()}});
        } else {
            // Ошибка от Bitget API
            result.success = false;
            result.error_message = "Bitget API: [" + code + "] " + msg;

            logger_->error(kComp, "Ордер отклонён биржей",
                {{"code", code}, {"msg", msg},
                 {"response", response.body.substr(0, 512)}});
        }

    } catch (const std::exception& ex) {
        result.success = false;
        result.error_message = std::string("Исключение: ") + ex.what();
        logger_->error(kComp, "Исключение при отправке ордера",
            {{"error", ex.what()}});
    }

    return result;
}

// ==================== cancel_order ====================

bool BitgetOrderSubmitter::cancel_order(const OrderId& order_id, const Symbol& symbol) {
    try {
        boost::json::object obj;
        obj["symbol"] = symbol.get();
        obj["orderId"] = order_id.get();

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Запрос отмены ордера",
            {{"order_id", order_id.get()}, {"symbol", symbol.get()}});

        auto response = rest_client_->post(kCancelOrderPath, body);

        if (!response.success) {
            logger_->error(kComp, "Отмена не выполнена (HTTP)",
                {{"error", response.error_message}});
            return false;
        }

        auto json = boost::json::parse(response.body);
        auto& resp_obj = json.as_object();
        std::string code = std::string(resp_obj.at("code").as_string());

        if (code == "00000") {
            logger_->info(kComp, "Ордер отменён",
                {{"order_id", order_id.get()}});
            return true;
        }

        std::string msg = resp_obj.contains("msg")
            ? std::string(resp_obj.at("msg").as_string()) : "";
        logger_->error(kComp, "Отмена отклонена биржей",
            {{"code", code}, {"msg", msg}});
        return false;

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Исключение при отмене ордера",
            {{"error", ex.what()}, {"order_id", order_id.get()}});
        return false;
    }
}

// ==================== query_order_fill_price ====================

Price BitgetOrderSubmitter::query_order_fill_price(
    const OrderId& exchange_order_id, const Symbol& symbol)
{
    try {
        // Bitget orderInfo API требует обязательный параметр symbol
        std::string query = "orderId=" + exchange_order_id.get()
                          + "&symbol=" + symbol.get();

        logger_->debug(kComp, "Запрос fill price для market ордера",
            {{"exchange_order_id", exchange_order_id.get()},
             {"symbol", symbol.get()}});

        auto response = rest_client_->get(kOrderInfoPath, query);

        if (!response.success) {
            logger_->warn(kComp, "Не удалось запросить fill price (HTTP)",
                {{"error", response.error_message}});
            return Price(0.0);
        }

        auto json = boost::json::parse(response.body);
        auto& obj = json.as_object();

        std::string code = std::string(obj.at("code").as_string());
        if (code != "00000") {
            std::string msg = obj.contains("msg")
                ? std::string(obj.at("msg").as_string()) : "";
            logger_->warn(kComp, "Bitget API ошибка при запросе fill price",
                {{"code", code}, {"msg", msg}});
            return Price(0.0);
        }

        auto& data_arr = obj.at("data").as_array();
        if (data_arr.empty()) {
            logger_->warn(kComp, "Пустой ответ orderInfo",
                {{"exchange_order_id", exchange_order_id.get()}});
            return Price(0.0);
        }

        auto& data = data_arr[0].as_object();

        // priceAvg — средняя цена исполнения
        if (data.contains("priceAvg") && !data.at("priceAvg").is_null()) {
            std::string price_str = std::string(data.at("priceAvg").as_string());
            double fill_price = std::stod(price_str);
            if (fill_price > 0.0) {
                logger_->info(kComp, "Получена реальная fill price",
                    {{"exchange_order_id", exchange_order_id.get()},
                     {"fill_price", price_str}});
                return Price(fill_price);
            }
        }

        logger_->debug(kComp, "priceAvg не доступен в ответе",
            {{"exchange_order_id", exchange_order_id.get()}});
        return Price(0.0);

    } catch (const std::exception& ex) {
        logger_->warn(kComp, "Исключение при запросе fill price",
            {{"error", ex.what()}, {"exchange_order_id", exchange_order_id.get()}});
        return Price(0.0);
    }
}

} // namespace tb::exchange::bitget
