#pragma once

/// @file bitget_futures_query_adapter.hpp
/// @brief Адаптер BitgetRestClient → IExchangeQueryService для фьючерсного reconciliation
///
/// Реализует интерфейс `reconciliation::IExchangeQueryService` поверх
/// `BitgetRestClient` для Bitget Mix API v2 (USDT-M Futures).
///
/// Ключевые отличия от Spot:
///  - Ордера: /api/v2/mix/order/orders-pending
///  - Баланс: /api/v2/mix/account/accounts (маржинальные аккаунты)
///  - Позиции: /api/v2/mix/position/all-position
///  - Информация о ордере: /api/v2/mix/order/detail

#include "reconciliation/reconciliation_engine.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "config/config_types.hpp"
#include "logging/logger.hpp"
#include <boost/json.hpp>
#include <memory>
#include <vector>
#include <string>

namespace tb::exchange::bitget {

/// Информация о фьючерсной позиции (расширение ExchangePositionInfo)
struct FuturesPositionInfo {
    Symbol symbol{Symbol("")};
    PositionSide position_side{PositionSide::Long};
    Quantity total{Quantity(0.0)};      ///< Общий размер позиции
    Quantity available{Quantity(0.0)};  ///< Доступно для закрытия
    Price entry_price{Price(0.0)};     ///< Средняя цена входа
    Price liquidation_price{Price(0.0)};
    Price mark_price{Price(0.0)};
    double unrealized_pnl{0.0};
    double margin{0.0};                ///< Занятая маржа
    int leverage{1};
    std::string margin_mode;           ///< "isolated" или "crossed"
};

/// Тип плана (TPSL/обычный stop)
enum class PlanOrderKind {
    Unknown,
    ProfitPlan,   ///< profit_plan / preset TP
    LossPlan,     ///< loss_plan / preset SL
    NormalPlan,   ///< обычный trigger plan order
    PosTPSL       ///< позиция-attached TPSL (pos_profit / pos_loss)
};

/// Информация о plan-ордере (Bitget orders-plan-pending)
struct PlanOrderInfo {
    OrderId order_id{OrderId("")};      ///< plan-orderId биржи
    OrderId client_order_id{OrderId("")};
    Symbol symbol{Symbol("")};
    PositionSide position_side{PositionSide::Long};
    PlanOrderKind kind{PlanOrderKind::Unknown};
    Price trigger_price{Price(0.0)};
    Price execute_price{Price(0.0)};    ///< 0 = market on trigger
    Quantity size{Quantity(0.0)};
    std::string trigger_type;           ///< "mark_price" / "fill_price"
    std::string plan_type;              ///< raw string from API
    int64_t created_at_ms{0};
};

/// Адаптер Bitget Mix (Futures) REST API → IExchangeQueryService
class BitgetFuturesQueryAdapter : public reconciliation::IExchangeQueryService {
public:
    BitgetFuturesQueryAdapter(
        std::shared_ptr<BitgetRestClient> rest_client,
        std::shared_ptr<logging::ILogger> logger,
        config::FuturesConfig futures_config);

    /// Получить открытые фьючерсные ордера
    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_open_orders(const Symbol& symbol = Symbol("")) override;

    /// Получить фьючерсные балансы (маржинальный аккаунт)
    Result<std::vector<reconciliation::ExchangePositionInfo>>
    get_account_balances() override;

    /// Получить открытые фьючерсные позиции в унифицированном формате
    Result<std::vector<reconciliation::ExchangeOpenPositionInfo>>
    get_open_positions(const Symbol& symbol = Symbol("")) override;

    /// Получить статус конкретного фьючерсного ордера
    Result<reconciliation::ExchangeOrderInfo>
    get_order_status(const OrderId& order_id, const Symbol& symbol) override;

    /// Получить все открытые фьючерсные позиции
    Result<std::vector<FuturesPositionInfo>> get_all_positions();

    /// Получить позиции по конкретному символу
    Result<std::vector<FuturesPositionInfo>> get_positions(const Symbol& symbol);

    /// Получить текущий funding rate для символа (USDT-M Futures).
    /// @return Funding rate как десятичная дробь (напр. 0.0001 = 0.01%).
    ///         Возвращает 0.0 при ошибке запроса.
    [[nodiscard]] double get_current_funding_rate(const Symbol& symbol);

    /// Получить историю ордеров (заполненные/отменённые) для reconciliation после рестарта.
    /// Endpoint: GET /api/v2/mix/order/orders-history
    /// @param symbol Фильтр по символу (пустой = все).
    /// @param limit Максимальное количество ордеров (по умолчанию 100, макс 100).
    /// @return Ордера, отсортированные от новых к старым.
    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_order_history(const Symbol& symbol = Symbol(""), int limit = 100);

    /// Получить серверное время Bitget (ms since epoch).
    /// Прокси к BitgetRestClient::get_server_time_ms().
    [[nodiscard]] int64_t get_server_time_ms();

    /// Получить открытые plan-ордера (preset TP/SL, trigger stop orders) для символа.
    /// Bitget endpoint: GET /api/v2/mix/order/orders-plan-pending
    /// @param symbol Фильтр по символу (пустой = все).
    /// @return Список plan-ордеров с их order_id, trigger_price, plan_type, position_side.
    /// Virtual для возможности override в unit-тестах ProtectiveBracketManager.
    [[nodiscard]] virtual Result<std::vector<PlanOrderInfo>>
    get_open_plan_orders(const Symbol& symbol = Symbol(""));

    /// Bug 2.1 fix: получить Open Interest для символа.
    /// Bitget endpoint: GET /api/v2/mix/market/open-interest
    /// @return OI в USDT-эквиваленте (holdingAmount × last_price). 0.0 при ошибке.
    [[nodiscard]] double get_open_interest_usdt(const Symbol& symbol);

private:
    /// Разобрать JSON-объект ордера → ExchangeOrderInfo
    [[nodiscard]] static reconciliation::ExchangeOrderInfo
    parse_order(const boost::json::object& obj);

    /// Разобрать JSON-объект позиции → FuturesPositionInfo
    [[nodiscard]] static FuturesPositionInfo
    parse_futures_position(const boost::json::object& obj);

    /// Разобрать JSON-объект plan-ордера → PlanOrderInfo
    [[nodiscard]] static PlanOrderInfo
    parse_plan_order(const boost::json::object& obj);

    /// Безопасно извлечь строковое поле из JSON-объекта
    [[nodiscard]] static std::string json_str(
        const boost::json::object& obj, std::string_view key);

    /// Безопасно извлечь числовое поле (double) из JSON-объекта
    [[nodiscard]] static double json_dbl(
        const boost::json::object& obj, std::string_view key);

    std::shared_ptr<BitgetRestClient>  rest_client_;
    std::shared_ptr<logging::ILogger>  logger_;
    config::FuturesConfig futures_config_;
};

} // namespace tb::exchange::bitget
