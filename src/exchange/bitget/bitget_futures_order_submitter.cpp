/**
 * @file bitget_futures_order_submitter.cpp
 * @brief Реализация отправки ордеров через Bitget Mix REST API v2 (USDT-M Futures)
 *
 * Логика позиций (hedge mode / double_hold):
 *  Long  open:  side="buy",  tradeSide="open"
 *  Long  close: side="sell", tradeSide="close"
 *  Short open:  side="sell", tradeSide="open"
 *  Short close: side="buy",  tradeSide="close"
 */

#include "bitget_futures_order_submitter.hpp"
#include "common/enums.hpp"

#include <boost/json.hpp>
#include <cmath>
#include <sstream>
#include <string>

namespace tb::exchange::bitget {

static constexpr char kComp[] = "FuturesSubmitter";

// Эндпоинты Bitget Mix v2 API (Futures)
static constexpr char kPlaceOrderPath[]   = "/api/v2/mix/order/place-order";
static constexpr char kCancelOrderPath[]  = "/api/v2/mix/order/cancel-order";
static constexpr char kOrderDetailPath[]  = "/api/v2/mix/order/detail";
static constexpr char kSetLeveragePath[]  = "/api/v2/mix/account/set-leverage";
static constexpr char kSetMarginPath[]    = "/api/v2/mix/account/set-margin-mode";
static constexpr char kSetHoldModePath[]  = "/api/v2/mix/account/set-position-mode";

// ==================== Конструктор ====================

BitgetFuturesOrderSubmitter::BitgetFuturesOrderSubmitter(
    std::shared_ptr<BitgetRestClient> rest_client,
    std::shared_ptr<logging::ILogger> logger,
    config::FuturesConfig futures_config)
    : rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , futures_config_(std::move(futures_config))
{
    logger_->info(kComp, "Bitget Futures Order Submitter создан",
        {{"product_type", futures_config_.product_type},
         {"margin_mode", futures_config_.margin_mode},
         {"margin_coin", futures_config_.margin_coin},
         {"default_leverage", std::to_string(futures_config_.default_leverage)}});
}

// ==================== Управление правилами символов ====================

void BitgetFuturesOrderSubmitter::set_rules(const Symbol& symbol, const ExchangeSymbolRules& rules) {
    std::lock_guard lock(rules_mutex_);
    rules_by_symbol_[symbol.get()] = rules;
    logger_->debug(kComp, "Правила символа установлены",
        {{"symbol", symbol.get()},
         {"price_precision", std::to_string(rules.price_precision)},
         {"quantity_precision", std::to_string(rules.quantity_precision)},
         {"min_trade_usdt", std::to_string(rules.min_trade_usdt)}});
}

const ExchangeSymbolRules& BitgetFuturesOrderSubmitter::rules(const Symbol& symbol) const {
    return get_rules(symbol);
}

const ExchangeSymbolRules& BitgetFuturesOrderSubmitter::get_rules(const Symbol& symbol) const {
    std::lock_guard lock(rules_mutex_);
    auto it = rules_by_symbol_.find(symbol.get());
    if (it != rules_by_symbol_.end()) {
        return it->second;
    }
    return default_rules_;
}

// ==================== Управление кредитным плечом на бирже ====================

bool BitgetFuturesOrderSubmitter::set_leverage(
    const Symbol& symbol, int leverage, const std::string& hold_side)
{
    // Кеш: если leverage уже установлен для этого символа+стороны — пропускаем API-вызов
    {
        std::string cache_key = symbol.get() + ":" + hold_side;
        std::lock_guard lock(leverage_cache_mutex_);

        // Ограничиваем запрошенный leverage известным максимумом символа
        auto max_it = max_leverage_cache_.find(symbol.get());
        if (max_it != max_leverage_cache_.end() && leverage > max_it->second) {
            leverage = max_it->second;
        }

        auto it = leverage_cache_.find(cache_key);
        if (it != leverage_cache_.end() && it->second == leverage) {
            return true;
        }
    }

    // Пробуем установить запрошенный leverage, при 40797 ("exceeded max")
    // автоматически снижаем до того, что примет биржа
    int attempt_leverage = leverage;
    while (attempt_leverage >= 1) {
        try {
            boost::json::object obj;
            obj["symbol"]      = symbol.get();
            obj["productType"] = futures_config_.product_type;
            obj["marginCoin"]  = futures_config_.margin_coin;
            obj["leverage"]    = std::to_string(attempt_leverage);

            if (futures_config_.margin_mode == "isolated" && !hold_side.empty()) {
                obj["holdSide"] = hold_side;
            }

            std::string body = boost::json::serialize(obj);

            logger_->info(kComp, "Установка leverage на бирже",
                {{"symbol", symbol.get()},
                 {"leverage", std::to_string(attempt_leverage)},
                 {"hold_side", hold_side}});

            auto response = rest_client_->post(kSetLeveragePath, body);

            // Bitget возвращает HTTP 400 + JSON body с code "40797" при превышении макс. leverage.
            // Парсим body даже при HTTP ошибке, чтобы определить можно ли снизить leverage.
            if (!response.success && !response.body.empty()) {
                try {
                    auto err_json = boost::json::parse(response.body);
                    auto& err_obj = err_json.as_object();
                    std::string err_code = std::string(err_obj.at("code").as_string());
                    if (err_code == "40797") {
                        logger_->warn(kComp, "Leverage слишком высокий для символа, снижаем",
                            {{"tried", std::to_string(attempt_leverage)},
                             {"symbol", symbol.get()}});
                        attempt_leverage = attempt_leverage / 2;
                        continue;
                    }
                } catch (...) {
                    // Не удалось распарсить body — обычная HTTP ошибка
                }
                logger_->error(kComp, "Ошибка установки leverage (HTTP)",
                    {{"error", response.error_message},
                     {"body", response.body}});
                return false;
            }
            if (!response.success) {
                logger_->error(kComp, "Ошибка установки leverage (HTTP)",
                    {{"error", response.error_message}});
                return false;
            }

            auto json = boost::json::parse(response.body);
            auto& resp_obj = json.as_object();
            std::string code = std::string(resp_obj.at("code").as_string());

            if (code == "00000") {
                if (attempt_leverage != leverage) {
                    logger_->warn(kComp, "Leverage снижен — биржа не поддерживает запрошенный",
                        {{"requested", std::to_string(leverage)},
                         {"actual", std::to_string(attempt_leverage)},
                         {"symbol", symbol.get()}});
                } else {
                    logger_->info(kComp, "Leverage установлен успешно",
                        {{"symbol", symbol.get()}, {"leverage", std::to_string(attempt_leverage)}});
                }
                // Обновляем кеш
                {
                    std::string cache_key = symbol.get() + ":" + hold_side;
                    std::lock_guard lock(leverage_cache_mutex_);
                    leverage_cache_[cache_key] = attempt_leverage;
                    // Запоминаем макс. leverage для символа (для будущих запросов)
                    if (attempt_leverage != leverage) {
                        max_leverage_cache_[symbol.get()] = attempt_leverage;
                    }
                }
                return true;
            }

            // 40797 = "Exceeded the maximum settable leverage" — пробуем ниже
            if (code == "40797") {
                logger_->warn(kComp, "Leverage слишком высокий для символа, снижаем",
                    {{"tried", std::to_string(attempt_leverage)},
                     {"symbol", symbol.get()}});
                // Бинарное снижение: 20->10->5->2->1
                attempt_leverage = attempt_leverage / 2;
                continue;
            }

            std::string msg = resp_obj.contains("msg")
                ? std::string(resp_obj.at("msg").as_string()) : "";
            logger_->error(kComp, "Bitget API: ошибка установки leverage",
                {{"code", code}, {"msg", msg}});
            return false;

        } catch (const std::exception& ex) {
            logger_->error(kComp, "Исключение при установке leverage",
                {{"error", ex.what()}, {"symbol", symbol.get()}});
            return false;
        }
    }

    logger_->error(kComp, "Не удалось установить leverage даже на 1x",
        {{"symbol", symbol.get()}});
    return false;
}

bool BitgetFuturesOrderSubmitter::set_margin_mode(
    const Symbol& symbol, const std::string& margin_mode)
{
    try {
        boost::json::object obj;
        obj["symbol"]      = symbol.get();
        obj["productType"] = futures_config_.product_type;
        obj["marginCoin"]  = futures_config_.margin_coin;
        obj["marginMode"]  = margin_mode;

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Установка margin mode на бирже",
            {{"symbol", symbol.get()}, {"margin_mode", margin_mode}});

        auto response = rest_client_->post(kSetMarginPath, body);

        if (!response.success) {
            logger_->error(kComp, "Ошибка установки margin mode (HTTP)",
                {{"error", response.error_message}});
            return false;
        }

        auto json = boost::json::parse(response.body);
        auto& resp_obj = json.as_object();
        std::string code = std::string(resp_obj.at("code").as_string());

        if (code == "00000") {
            logger_->info(kComp, "Margin mode установлен",
                {{"symbol", symbol.get()}, {"margin_mode", margin_mode}});
            return true;
        }

        std::string msg = resp_obj.contains("msg")
            ? std::string(resp_obj.at("msg").as_string()) : "";

        // Код 40872 = "Margin mode is the same" — не ошибка
        if (code == "40872") {
            logger_->debug(kComp, "Margin mode уже установлен", {{"symbol", symbol.get()}});
            return true;
        }

        logger_->error(kComp, "Bitget API: ошибка установки margin mode",
            {{"code", code}, {"msg", msg}});
        return false;

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Исключение при установке margin mode",
            {{"error", ex.what()}, {"symbol", symbol.get()}});
        return false;
    }
}

bool BitgetFuturesOrderSubmitter::set_hold_mode(
    const std::string& product_type, const std::string& hold_mode)
{
    try {
        boost::json::object obj;
        obj["productType"] = product_type;
        obj["posMode"]     = hold_mode;

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Установка hold mode",
            {{"productType", product_type}, {"holdMode", hold_mode}});

        auto response = rest_client_->post(kSetHoldModePath, body);

        if (!response.success) {
            logger_->error(kComp, "Ошибка установки hold mode (HTTP)",
                {{"error", response.error_message}});
            return false;
        }

        auto json = boost::json::parse(response.body);
        auto& resp_obj = json.as_object();
        std::string code = std::string(resp_obj.at("code").as_string());

        if (code == "00000") {
            logger_->info(kComp, "Hold mode установлен", {{"holdMode", hold_mode}});
            return true;
        }

        std::string msg = resp_obj.contains("msg")
            ? std::string(resp_obj.at("msg").as_string()) : "";

        // Код 40871 = "Hold mode is the same" — не ошибка
        if (code == "40871") {
            logger_->debug(kComp, "Hold mode уже установлен", {{"holdMode", hold_mode}});
            return true;
        }

        logger_->error(kComp, "Bitget API: ошибка установки hold mode",
            {{"code", code}, {"msg", msg}});
        return false;

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Исключение при установке hold mode",
            {{"error", ex.what()}});
        return false;
    }
}

// ==================== Формирование JSON тела фьючерсного ордера ====================

std::string BitgetFuturesOrderSubmitter::build_place_order_json(
    const execution::OrderRecord& order) const
{
    const auto& sym_rules = get_rules(order.symbol);

    boost::json::object obj;

    // Обязательные фьючерсные поля
    obj["symbol"]      = order.symbol.get();
    obj["productType"] = futures_config_.product_type;
    obj["marginMode"]  = futures_config_.margin_mode;
    obj["marginCoin"]  = futures_config_.margin_coin;

    // Идемпотентный clientOid
    if (!order.execution_info.client_order_id.empty()) {
        obj["clientOid"] = order.execution_info.client_order_id;
    }

    // === Направление (КРИТИЧНО для фьючерсов) ===
    // В hedge-mode "side" указывает НАПРАВЛЕНИЕ ПОЗИЦИИ, не действие:
    //   side=buy  → позиция Long
    //   side=sell → позиция Short
    // "tradeSide" указывает открытие/закрытие:
    //   tradeSide=open  → открытие позиции
    //   tradeSide=close → закрытие позиции
    //
    // Long open:  side=buy,  tradeSide=open
    // Long close: side=buy,  tradeSide=close
    // Short open: side=sell, tradeSide=open
    // Short close: side=sell, tradeSide=close
    std::string api_side;
    std::string api_trade_side;

    if (order.position_side == PositionSide::Long) {
        api_side = "buy";  // Long position direction
        if (order.trade_side == TradeSide::Open) {
            api_trade_side = "open";
        } else {
            api_trade_side = "close";
        }
    } else { // Short
        api_side = "sell";  // Short position direction
        if (order.trade_side == TradeSide::Open) {
            api_trade_side = "open";
        } else {
            api_trade_side = "close";
        }
    }

    obj["side"]      = api_side;
    obj["tradeSide"] = api_trade_side;

    // NOTE: reduceOnly НЕ отправляется в hedge mode.
    // Bitget docs: "reduceOnly — Applicable only in one-way-position mode".
    // В hedge mode закрытие позиции определяется через tradeSide=close.

    // === Тип ордера и force ===
    bool is_market    = (order.order_type == OrderType::Market);
    bool is_post_only = (order.order_type == OrderType::PostOnly);
    bool is_limit     = (order.order_type == OrderType::Limit);

    if (order.order_type == OrderType::StopMarket || order.order_type == OrderType::StopLimit) {
        logger_->error(kComp, "Стоп-ордера отправляются через отдельный эндпоинт plan-order",
            {{"order_type", std::string(to_string(order.order_type))}});
        return "{}";
    }

    if (is_market) {
        obj["orderType"] = "market";
    } else if (is_post_only) {
        obj["orderType"] = "limit";
        obj["force"]     = "post_only";
    } else if (is_limit) {
        obj["orderType"] = "limit";
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

    // === Размер ордера ===
    // Для фьючерсов size ВСЕГДА в base currency (контрактах / монетах)
    double floored_qty = sym_rules.floor_quantity(order.original_quantity.get());
    if (!sym_rules.is_quantity_valid(floored_qty)) {
        logger_->error(kComp, "Ордер отклонён: quantity невалиден после округления",
            {{"original_qty", std::to_string(order.original_quantity.get())},
             {"floored_qty", std::to_string(floored_qty)},
             {"symbol", order.symbol.get()}});
        return "{}";
    }
    obj["size"] = sym_rules.format_quantity(floored_qty);

    // === Цена — только для лимитных ===
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

execution::OrderSubmitResult BitgetFuturesOrderSubmitter::submit_order(
    const execution::OrderRecord& order)
{
    execution::OrderSubmitResult result;
    result.order_id = order.order_id;

    try {
        std::string body = build_place_order_json(order);

        if (body == "{}") {
            result.success = false;
            result.error_message = "Ошибка формирования JSON фьючерсного ордера";
            return result;
        }

        // Запомнить реальное кол-во после floor (для корректного fill tracking)
        const auto& sym_rules = get_rules(order.symbol);
        double floored = sym_rules.floor_quantity(order.original_quantity.get());
        result.submitted_quantity = Quantity(floored);

        logger_->info(kComp, "Отправка фьючерсного ордера на биржу",
            {{"symbol", order.symbol.get()},
             {"side", std::string(to_string(order.side))},
             {"position_side", std::string(to_string(order.position_side))},
             {"trade_side", std::string(to_string(order.trade_side))},
             {"type", std::string(to_string(order.order_type))},
             {"qty", std::to_string(order.original_quantity.get())},
             {"price", std::to_string(order.price.get())},
             {"product_type", futures_config_.product_type}});

        auto response = rest_client_->post(kPlaceOrderPath, body);

        if (!response.success) {
            result.success = false;
            result.error_message = "HTTP ошибка: " + response.error_message;
            logger_->error(kComp, "Фьючерсный ордер не отправлен (HTTP)",
                {{"error", response.error_message},
                 {"body", response.body.substr(0, 512)}});
            return result;
        }

        auto json = boost::json::parse(response.body);
        auto& obj = json.as_object();

        std::string code = std::string(obj.at("code").as_string());
        std::string msg = obj.contains("msg")
            ? std::string(obj.at("msg").as_string()) : "";

        if (code == "00000") {
            auto& data = obj.at("data").as_object();
            std::string exchange_id = std::string(data.at("orderId").as_string());

            result.success = true;
            result.exchange_order_id = OrderId(exchange_id);

            logger_->info(kComp, "Фьючерсный ордер принят биржей",
                {{"exchange_order_id", exchange_id},
                 {"internal_order_id", order.order_id.get()},
                 {"position_side", std::string(to_string(order.position_side))},
                 {"trade_side", std::string(to_string(order.trade_side))}});
        } else {
            result.success = false;
            result.error_message = "Bitget Futures API: [" + code + "] " + msg;

            logger_->error(kComp, "Фьючерсный ордер отклонён биржей",
                {{"code", code}, {"msg", msg},
                 {"response", response.body.substr(0, 512)}});
        }

    } catch (const std::exception& ex) {
        result.success = false;
        result.error_message = std::string("Исключение: ") + ex.what();
        logger_->error(kComp, "Исключение при отправке фьючерсного ордера",
            {{"error", ex.what()}});
    }

    return result;
}

// ==================== cancel_order ====================

bool BitgetFuturesOrderSubmitter::cancel_order(const OrderId& order_id, const Symbol& symbol) {
    try {
        boost::json::object obj;
        obj["symbol"]      = symbol.get();
        obj["productType"] = futures_config_.product_type;
        obj["marginCoin"]  = futures_config_.margin_coin;
        obj["orderId"]     = order_id.get();

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Запрос отмены фьючерсного ордера",
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
            logger_->info(kComp, "Фьючерсный ордер отменён",
                {{"order_id", order_id.get()}});
            return true;
        }

        std::string msg = resp_obj.contains("msg")
            ? std::string(resp_obj.at("msg").as_string()) : "";
        logger_->error(kComp, "Отмена фьючерсного ордера отклонена биржей",
            {{"code", code}, {"msg", msg}});
        return false;

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Исключение при отмене фьючерсного ордера",
            {{"error", ex.what()}, {"order_id", order_id.get()}});
        return false;
    }
}

// ==================== query_order_fill_price ====================

Price BitgetFuturesOrderSubmitter::query_order_fill_price(
    const OrderId& exchange_order_id, const Symbol& symbol)
{
    try {
        std::string query = "symbol=" + symbol.get()
                          + "&productType=" + futures_config_.product_type
                          + "&orderId=" + exchange_order_id.get();

        logger_->debug(kComp, "Запрос fill price для фьючерсного ордера",
            {{"exchange_order_id", exchange_order_id.get()},
             {"symbol", symbol.get()}});

        auto response = rest_client_->get(kOrderDetailPath, query);

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
            logger_->warn(kComp, "Bitget Futures API ошибка при запросе fill price",
                {{"code", code}, {"msg", msg}});
            return Price(0.0);
        }

        // Mix API v2 detail может вернуть data как объект или массив
        const auto& data_val = obj.at("data");
        const boost::json::object* data_ptr = nullptr;

        if (data_val.is_object()) {
            data_ptr = &data_val.as_object();
        } else if (data_val.is_array() && !data_val.as_array().empty()) {
            data_ptr = &data_val.as_array()[0].as_object();
        }

        if (!data_ptr) {
            logger_->warn(kComp, "Пустой ответ order detail",
                {{"exchange_order_id", exchange_order_id.get()}});
            return Price(0.0);
        }

        const auto& data = *data_ptr;

        // priceAvg — средняя цена исполнения
        auto it = data.find("priceAvg");
        if (it != data.end() && !it->value().is_null()) {
            std::string price_str;
            if (it->value().is_string()) {
                price_str = std::string(it->value().as_string());
            }
            if (!price_str.empty()) {
                double fill_price = std::stod(price_str);
                if (fill_price > 0.0) {
                    logger_->info(kComp, "Получена реальная fill price (фьючерсы)",
                        {{"exchange_order_id", exchange_order_id.get()},
                         {"fill_price", price_str}});
                    return Price(fill_price);
                }
            }
        }

        logger_->debug(kComp, "priceAvg не доступен в ответе",
            {{"exchange_order_id", exchange_order_id.get()}});
        return Price(0.0);

    } catch (const std::exception& ex) {
        logger_->warn(kComp, "Исключение при запросе fill price (фьючерсы)",
            {{"error", ex.what()}, {"exchange_order_id", exchange_order_id.get()}});
        return Price(0.0);
    }
}

// ==================== query_order_fill_detail ====================

execution::OrderFillDetail BitgetFuturesOrderSubmitter::query_order_fill_detail(
    const OrderId& exchange_order_id, const Symbol& symbol)
{
    execution::OrderFillDetail result;
    try {
        std::string query = "symbol=" + symbol.get()
                          + "&productType=" + futures_config_.product_type
                          + "&orderId=" + exchange_order_id.get();

        auto response = rest_client_->get(kOrderDetailPath, query);
        if (!response.success) {
            logger_->warn(kComp, "Не удалось запросить order detail для подтверждения fill",
                {{"error", response.error_message}});
            return result;
        }

        auto json = boost::json::parse(response.body);
        auto& obj = json.as_object();

        std::string code = std::string(obj.at("code").as_string());
        if (code != "00000") {
            logger_->warn(kComp, "Bitget API ошибка при запросе order detail",
                {{"code", code}});
            return result;
        }

        const auto& data_val = obj.at("data");
        const boost::json::object* data_ptr = nullptr;
        if (data_val.is_object()) {
            data_ptr = &data_val.as_object();
        } else if (data_val.is_array() && !data_val.as_array().empty()) {
            data_ptr = &data_val.as_array()[0].as_object();
        }
        if (!data_ptr) return result;

        const auto& data = *data_ptr;

        // Парсим status
        if (auto it = data.find("state"); it != data.end() && it->value().is_string()) {
            result.status = std::string(it->value().as_string());
        } else if (auto it2 = data.find("status"); it2 != data.end() && it2->value().is_string()) {
            result.status = std::string(it2->value().as_string());
        }

        // Парсим priceAvg
        if (auto it = data.find("priceAvg"); it != data.end() && it->value().is_string()) {
            std::string s(it->value().as_string());
            if (!s.empty()) result.fill_price = Price(std::stod(s));
        }

        // Парсим baseVolume / filledQty (исполненное количество)
        for (const char* field : {"baseVolume", "filledQty", "fillQuantity"}) {
            if (auto it = data.find(field); it != data.end() && it->value().is_string()) {
                std::string s(it->value().as_string());
                if (!s.empty()) {
                    double v = std::stod(s);
                    if (v > 0.0) { result.filled_qty = Quantity(v); break; }
                }
            }
        }

        // Парсим size (объём ордера)
        if (auto it = data.find("size"); it != data.end() && it->value().is_string()) {
            std::string s(it->value().as_string());
            if (!s.empty()) result.original_qty = Quantity(std::stod(s));
        }

        result.success = (result.fill_price.get() > 0.0 || result.filled_qty.get() > 0.0);

        logger_->info(kComp, "Order fill detail получен",
            {{"exchange_order_id", exchange_order_id.get()},
             {"status", result.status},
             {"fill_price", std::to_string(result.fill_price.get())},
             {"filled_qty", std::to_string(result.filled_qty.get())}});

        return result;

    } catch (const std::exception& ex) {
        logger_->warn(kComp, "Исключение при запросе order fill detail",
            {{"error", ex.what()}, {"exchange_order_id", exchange_order_id.get()}});
        return result;
    }
}

} // namespace tb::exchange::bitget
