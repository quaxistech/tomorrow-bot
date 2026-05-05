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

#include <algorithm>
#include <boost/json.hpp>
#include <cctype>
#include <cmath>
#include <random>
#include <sstream>
#include <string>
#include <thread>

namespace tb::exchange::bitget {

static constexpr char kComp[] = "FuturesSubmitter";

/// Anti-fingerprinting: random delay before order submission.
/// Prevents timing pattern detection by introducing 20-150ms jitter.
/// Thread-local RNG — no mutex needed.
static void apply_submission_jitter() {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(20, 150);
    int delay_ms = dist(rng);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

static bool should_apply_submission_jitter(const execution::OrderRecord& order) {
    if (order.trade_side != TradeSide::Open) {
        return false;
    }

    std::string strategy = order.strategy_id.get();
    std::transform(strategy.begin(), strategy.end(), strategy.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (strategy.find("hedge") != std::string::npos ||
        strategy.find("emergency") != std::string::npos ||
        strategy.find("flatten") != std::string::npos ||
        strategy.find("stop") != std::string::npos) {
        return false;
    }

    return true;
}

// Эндпоинты Bitget Mix v2 API (Futures)
static constexpr char kPlaceOrderPath[]     = "/api/v2/mix/order/place-order";
static constexpr char kPlacePlanOrderPath[] = "/api/v2/mix/order/place-plan-order";
static constexpr char kCancelOrderPath[]    = "/api/v2/mix/order/cancel-order";
static constexpr char kCancelPlanPath[]     = "/api/v2/mix/order/cancel-plan-order";
static constexpr char kOrderDetailPath[]    = "/api/v2/mix/order/detail";
static constexpr char kSetLeveragePath[]    = "/api/v2/mix/account/set-leverage";
static constexpr char kSetMarginPath[]      = "/api/v2/mix/account/set-margin-mode";
static constexpr char kSetHoldModePath[]    = "/api/v2/mix/account/set-position-mode";

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
                        attempt_leverage -= 1;
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

            // 40797 = "Exceeded the maximum settable leverage" — пробуем ниже.
            // Декремент на 1 гарантирует нахождение точного максимума: бинарное деление
            // пропускало значения (40→20→10 при max=15 давало 10 вместо 15).
            if (code == "40797") {
                logger_->warn(kComp, "Leverage слишком высокий для символа, снижаем",
                    {{"tried", std::to_string(attempt_leverage)},
                     {"symbol", symbol.get()}});
                attempt_leverage -= 1;
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
    // Bitget Mix API v2 hedge-mode (double_hold):
    //   Long  open:  side="buy",  tradeSide="open"
    //   Long  close: side="sell", tradeSide="close"   ← sell to close long
    //   Short open:  side="sell", tradeSide="open"
    //   Short close: side="buy",  tradeSide="close"   ← buy to close short
    //
    // "side" is the ORDER direction (buy=long-side, sell=short-side), not the
    // position being held.  For closing, you must order in the OPPOSITE direction.

    // Defensive validation: reject if position_side or trade_side are out of enum range.
    // Prevents silent position inversion from uninitialized/corrupted enum values.
    if (order.position_side != PositionSide::Long &&
        order.position_side != PositionSide::Short) {
        logger_->error(kComp, "Невалидный position_side enum — ордер отклонён",
            {{"value", std::to_string(static_cast<int>(order.position_side))},
             {"symbol", order.symbol.get()}});
        return "{}";
    }
    if (order.trade_side != TradeSide::Open &&
        order.trade_side != TradeSide::Close) {
        logger_->error(kComp, "Невалидный trade_side enum — ордер отклонён",
            {{"value", std::to_string(static_cast<int>(order.trade_side))},
             {"symbol", order.symbol.get()}});
        return "{}";
    }

    std::string api_side;
    std::string api_trade_side;

    // BUG-S9-01: closing a position requires the opposite side direction.
    if (order.position_side == PositionSide::Long) {
        api_side      = (order.trade_side == TradeSide::Open) ? "buy"  : "sell";
        api_trade_side = (order.trade_side == TradeSide::Open) ? "open" : "close";
    } else { // Short
        api_side      = (order.trade_side == TradeSide::Open) ? "sell" : "buy";
        api_trade_side = (order.trade_side == TradeSide::Open) ? "open" : "close";
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
        logger_->warn(kComp, "Стоп-ордера маршрутизируются через submit_plan_order()",
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

    // === Attached TP/SL (Bitget exchange-level protection) ===
    if (order.attached_tp_sl.has_any()) {
        if (order.attached_tp_sl.has_tp()) {
            obj["presetStopSurplusPrice"] =
                sym_rules.format_price(order.attached_tp_sl.stop_surplus_price.get());
        }
        if (order.attached_tp_sl.has_sl()) {
            obj["presetStopLossPrice"] =
                sym_rules.format_price(order.attached_tp_sl.stop_loss_price.get());
        }
    }

    // === Self-Trade Prevention (STP) ===
    // Prevents matching against our own resting orders in paired execution.
    // "cancel_taker" = incoming taker order is cancelled if it would self-trade.
    obj["stpMode"] = "cancel_taker";

    return boost::json::serialize(obj);
}

// ==================== submit_order ====================

execution::OrderSubmitResult BitgetFuturesOrderSubmitter::submit_order(
    const execution::OrderRecord& order)
{
    execution::OrderSubmitResult result;
    result.order_id = order.order_id;

    // Route StopMarket/StopLimit to plan order endpoint
    if (order.order_type == OrderType::StopMarket || order.order_type == OrderType::StopLimit) {
        return submit_plan_order(order);
    }

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

        // Anti-fingerprinting is allowed only for non-protective opening orders.
        if (should_apply_submission_jitter(order)) {
            apply_submission_jitter();
        }

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

            // Classify API error for structured handling
            if (code == "40768" || code == "40769") {
                // Insufficient margin / would trigger liquidation — not retryable
                logger_->error(kComp, "Ордер отклонён: недостаточная маржа",
                    {{"code", code}, {"msg", msg}, {"symbol", order.symbol.get()}});
            } else if (code == "40773") {
                // Quantity too small — not retryable
                logger_->error(kComp, "Ордер отклонён: количество слишком мало",
                    {{"code", code}, {"msg", msg}, {"symbol", order.symbol.get()}});
            } else if (code == "40780") {
                // Position doesn't exist (trying to close non-existent position)
                logger_->error(kComp, "Ордер отклонён: позиция не найдена",
                    {{"code", code}, {"msg", msg}, {"symbol", order.symbol.get()}});
            } else if (code == "40872") {
                // Margin mode already set — treat as non-fatal
                logger_->warn(kComp, "Margin mode уже установлен (не критично)",
                    {{"code", code}, {"msg", msg}});
            } else if (code == "45100" || code == "45101") {
                // System/network busy — transient, logged for retry at caller level
                logger_->warn(kComp, "Биржа занята, ордер не принят (transient)",
                    {{"code", code}, {"msg", msg}, {"symbol", order.symbol.get()}});
            } else {
                logger_->error(kComp, "Фьючерсный ордер отклонён биржей",
                    {{"code", code}, {"msg", msg},
                     {"response", response.body.substr(0, 512)}});
            }
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

        // M-25 fix: "already filled" or "order does not exist" is not a failure —
        // the order is in a terminal state, treat cancel as success.
        // Bitget codes: 43011 = order already filled/cancelled, 43012 = order not found
        if (code == "43011" || code == "43012") {
            logger_->info(kComp, "Ордер уже в терминальном состоянии, отмена не требуется",
                {{"order_id", order_id.get()}, {"code", code}});
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

// ==================== build_plan_order_json ====================

std::string BitgetFuturesOrderSubmitter::build_plan_order_json(
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

    // Направление (та же логика что и для place-order)
    if (order.position_side == PositionSide::Long) {
        obj["side"] = "buy";
    } else {
        obj["side"] = "sell";
    }
    obj["tradeSide"] = (order.trade_side == TradeSide::Open) ? "open" : "close";

    // Plan type: profit_plan (TP) или loss_plan (SL)
    // Определяем из типа ордера и контекста:
    //   - StopMarket/StopLimit при закрытии Long с trigger ниже текущей = loss_plan
    //   - StopMarket/StopLimit при закрытии Long с trigger выше текущей = profit_plan
    // Для простоты: если trigger_price > 0 и order_type == StopMarket → market execution
    //               если order_type == StopLimit → limit execution

    // Trigger price
    if (order.plan_params.trigger_price.get() <= 0.0) {
        logger_->error(kComp, "Plan ордер без trigger price",
            {{"order_id", order.order_id.get()}});
        return "{}";
    }
    obj["triggerPrice"] = sym_rules.format_price(order.plan_params.trigger_price.get());

    // Trigger type: fill_price (last trade) или mark_price
    obj["triggerType"] = (order.plan_params.trigger_type == execution::TriggerType::MarkPrice)
        ? "mark_price" : "fill_price";

    // Размер ордера
    double floored_qty = sym_rules.floor_quantity(order.original_quantity.get());
    if (!sym_rules.is_quantity_valid(floored_qty)) {
        logger_->error(kComp, "Plan ордер: quantity невалиден после округления",
            {{"original_qty", std::to_string(order.original_quantity.get())},
             {"floored_qty", std::to_string(floored_qty)},
             {"symbol", order.symbol.get()}});
        return "{}";
    }
    obj["size"] = sym_rules.format_quantity(floored_qty);

    // Тип исполнения при срабатывании
    if (order.order_type == OrderType::StopMarket) {
        obj["orderType"] = "market";
    } else if (order.order_type == OrderType::StopLimit) {
        obj["orderType"] = "limit";
        if (order.plan_params.execute_price.get() > 0.0) {
            obj["executePrice"] = sym_rules.format_price(order.plan_params.execute_price.get());
        } else if (order.price.get() > 0.0) {
            obj["executePrice"] = sym_rules.format_price(order.price.get());
        } else {
            logger_->error(kComp, "StopLimit plan ордер без execute price",
                {{"order_id", order.order_id.get()}});
            return "{}";
        }
    } else {
        logger_->error(kComp, "Некорректный тип ордера для plan order",
            {{"order_type", std::string(to_string(order.order_type))}});
        return "{}";
    }

    // Self-Trade Prevention for plan/TPSL orders
    obj["stpMode"] = "cancel_taker";

    return boost::json::serialize(obj);
}

// ==================== submit_plan_order ====================

execution::OrderSubmitResult BitgetFuturesOrderSubmitter::submit_plan_order(
    const execution::OrderRecord& order)
{
    execution::OrderSubmitResult result;
    result.order_id = order.order_id;

    try {
        std::string body = build_plan_order_json(order);

        if (body == "{}") {
            result.success = false;
            result.error_message = "Ошибка формирования JSON plan ордера";
            return result;
        }

        const auto& sym_rules = get_rules(order.symbol);
        double floored = sym_rules.floor_quantity(order.original_quantity.get());
        result.submitted_quantity = Quantity(floored);

        logger_->info(kComp, "Отправка plan ордера на биржу",
            {{"symbol", order.symbol.get()},
             {"side", std::string(to_string(order.side))},
             {"position_side", std::string(to_string(order.position_side))},
             {"trade_side", std::string(to_string(order.trade_side))},
             {"type", std::string(to_string(order.order_type))},
             {"trigger_price", std::to_string(order.plan_params.trigger_price.get())},
             {"qty", std::to_string(order.original_quantity.get())}});

        auto response = rest_client_->post(kPlacePlanOrderPath, body);

        if (!response.success) {
            result.success = false;
            result.error_message = "HTTP ошибка: " + response.error_message;
            logger_->error(kComp, "Plan ордер не отправлен (HTTP)",
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

            logger_->info(kComp, "Plan ордер принят биржей",
                {{"exchange_order_id", exchange_id},
                 {"internal_order_id", order.order_id.get()},
                 {"trigger_price", std::to_string(order.plan_params.trigger_price.get())}});
        } else {
            result.success = false;
            result.error_message = "Bitget Futures API: [" + code + "] " + msg;

            logger_->error(kComp, "Plan ордер отклонён биржей",
                {{"code", code}, {"msg", msg},
                 {"response", response.body.substr(0, 512)}});
        }

    } catch (const std::exception& ex) {
        result.success = false;
        result.error_message = std::string("Исключение: ") + ex.what();
        logger_->error(kComp, "Исключение при отправке plan ордера",
            {{"error", ex.what()}});
    }

    return result;
}

// ==================== cancel_plan_order ====================

bool BitgetFuturesOrderSubmitter::cancel_plan_order(
    const OrderId& order_id, const Symbol& symbol)
{
    try {
        boost::json::object obj;
        obj["symbol"]      = symbol.get();
        obj["productType"] = futures_config_.product_type;
        obj["marginCoin"]  = futures_config_.margin_coin;
        obj["orderId"]     = order_id.get();

        std::string body = boost::json::serialize(obj);

        logger_->info(kComp, "Запрос отмены plan ордера",
            {{"order_id", order_id.get()}, {"symbol", symbol.get()}});

        auto response = rest_client_->post(kCancelPlanPath, body);

        if (!response.success) {
            logger_->error(kComp, "Отмена plan ордера не выполнена (HTTP)",
                {{"error", response.error_message}});
            return false;
        }

        auto json = boost::json::parse(response.body);
        auto& resp_obj = json.as_object();
        std::string code = std::string(resp_obj.at("code").as_string());

        if (code == "00000") {
            logger_->info(kComp, "Plan ордер отменён",
                {{"order_id", order_id.get()}});
            return true;
        }

        std::string msg = resp_obj.contains("msg")
            ? std::string(resp_obj.at("msg").as_string()) : "";
        logger_->error(kComp, "Отмена plan ордера отклонена биржей",
            {{"code", code}, {"msg", msg}});
        return false;

    } catch (const std::exception& ex) {
        logger_->error(kComp, "Исключение при отмене plan ордера",
            {{"error", ex.what()}, {"order_id", order_id.get()}});
        return false;
    }
}

} // namespace tb::exchange::bitget
