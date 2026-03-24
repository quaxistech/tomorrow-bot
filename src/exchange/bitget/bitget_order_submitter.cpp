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

// ==================== Формирование JSON тела запроса ====================

std::string BitgetOrderSubmitter::build_place_order_json(
    const execution::OrderRecord& order) const
{
    boost::json::object obj;

    obj["symbol"] = order.symbol.get();

    // Направление: buy / sell
    obj["side"] = std::string(to_string(order.side));

    // Тип ордера и force
    bool is_market = (order.order_type == OrderType::Market);
    bool is_post_only = (order.order_type == OrderType::PostOnly);

    if (is_market) {
        obj["orderType"] = "market";
        // Для market ордеров force не отправляем — Bitget его не принимает
    } else if (is_post_only) {
        obj["orderType"] = "limit";
        obj["force"] = "post_only";
    } else {
        // Limit и остальные — как limit
        obj["orderType"] = "limit";

        // TimeInForce → force
        switch (order.tif) {
            case TimeInForce::GoodTillCancel:    obj["force"] = "gtc"; break;
            case TimeInForce::ImmediateOrCancel: obj["force"] = "ioc"; break;
            case TimeInForce::FillOrKill:        obj["force"] = "fok"; break;
            default:                             obj["force"] = "gtc"; break;
        }
    }

    // Размер ордера
    // Для market buy — size = сумма в USDT (quote currency)
    // Для market sell — size = количество в BTC (base currency)
    // Для limit — size = количество в BTC (base currency)
    if (is_market && order.side == Side::Buy) {
        // Рыночная покупка: size = сколько USDT потратить.
        // original_quantity в base currency (BTC), price — оценочная рыночная цена.
        // Если price задана, конвертируем base → quote. Иначе используем quantity как USDT.
        double quote_amount = 0.0;
        if (order.price.get() > 0.0) {
            quote_amount = order.original_quantity.get() * order.price.get();
        }
        if (quote_amount <= 0.0) {
            // Fallback: если цена не задана, используем quantity как USDT напрямую.
            // Это корректно только если вызывающий код задал qty уже в USDT.
            quote_amount = order.original_quantity.get();
            // Защита: если quote_amount слишком мал (< $1), скорее всего это base qty
            // без цены — логируем предупреждение
            if (quote_amount < 1.0) {
                logger_->warn(kComp, "Market buy: quote_amount подозрительно мал, возможно base qty без цены",
                    {{"quote_amount", std::to_string(quote_amount)}});
            }
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(price_scale_) << quote_amount;
        obj["size"] = oss.str();
    } else {
        // Limit или market sell: size = количество base currency.
        // Используем quantity_scale_ из exchange info (checkScale для данного символа).
        // floor (обрезание вниз), чтобы не превысить реальный баланс.
        double scale_factor = std::pow(10.0, quantity_scale_);
        double base_qty = std::floor(order.original_quantity.get() * scale_factor) / scale_factor;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(quantity_scale_) << base_qty;
        obj["size"] = oss.str();
    }

    // Цена — только для лимитных ордеров
    // Используем price_scale_ из exchange info (quotePrecision для данного символа).
    if (!is_market) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(price_scale_) << order.price.get();
        obj["price"] = oss.str();
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
        last_order_symbol_ = order.symbol.get();  // Запоминаем символ для cancel_order

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

bool BitgetOrderSubmitter::cancel_order(const OrderId& order_id) {
    try {
        boost::json::object obj;
        // Используем символ последнего отправленного ордера (т.к. interface IOrderSubmitter
        // не передаёт symbol в cancel_order). Бот торгует одним символом за раз.
        obj["symbol"] = last_order_symbol_;
        obj["orderId"] = order_id.get();

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Запрос отмены ордера",
            {{"order_id", order_id.get()}});

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

} // namespace tb::exchange::bitget
