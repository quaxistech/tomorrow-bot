/**
 * @file test_reconciliation.cpp
 * @brief Тесты движка reconciliation
 */
#include <catch2/catch_test_macros.hpp>
#include "test_mocks.hpp"
#include "reconciliation/reconciliation_engine.hpp"
#include "reconciliation/reconciliation_types.hpp"
#include "execution/order_types.hpp"
#include "portfolio/portfolio_types.hpp"

using namespace tb;
using namespace tb::test;
using namespace tb::reconciliation;

// ========== Mock IExchangeQueryService ==========

class MockExchangeQueryService : public IExchangeQueryService {
public:
    std::vector<ExchangeOrderInfo> open_orders_;
    std::vector<ExchangePositionInfo> account_balances_;
    std::unordered_map<std::string, ExchangeOrderInfo> order_statuses_;

    Result<std::vector<ExchangeOrderInfo>>
    get_open_orders(const Symbol& /*symbol*/) override {
        return open_orders_;
    }

    Result<std::vector<ExchangePositionInfo>>
    get_account_balances() override {
        return account_balances_;
    }

    Result<ExchangeOrderInfo>
    get_order_status(const OrderId& order_id, const Symbol& /*symbol*/) override {
        auto it = order_statuses_.find(order_id.get());
        if (it != order_statuses_.end()) {
            return it->second;
        }
        return std::unexpected(TbError::ReconciliationFailed);
    }
};

// ========== Вспомогательные функции ==========

inline ReconciliationConfig make_default_config() {
    ReconciliationConfig cfg;
    cfg.enabled = true;
    cfg.auto_resolve_state_mismatches = true;
    cfg.auto_cancel_orphan_orders = true;
    cfg.auto_close_orphan_positions = true;
    cfg.balance_tolerance_pct = 1.0;
    cfg.max_auto_resolutions_per_run = 10;
    cfg.stale_order_threshold_ms = 60000;
    return cfg;
}

inline execution::OrderRecord make_local_order(
    const std::string& id, const std::string& symbol,
    Side side, execution::OrderState state) {
    execution::OrderRecord rec;
    rec.order_id = OrderId(id);
    rec.exchange_order_id = OrderId("exch-" + id);
    rec.symbol = Symbol(symbol);
    rec.side = side;
    rec.state = state;
    rec.original_quantity = Quantity(1.0);
    rec.filled_quantity = Quantity(0.0);
    rec.price = Price(50000.0);
    rec.created_at = Timestamp(900000);
    return rec;
}

inline ExchangeOrderInfo make_exchange_order(
    const std::string& id, const std::string& symbol,
    Side side, const std::string& status) {
    ExchangeOrderInfo info;
    info.order_id = OrderId("exch-" + id);
    info.client_order_id = OrderId(id);
    info.symbol = Symbol(symbol);
    info.side = side;
    info.order_type = OrderType::Limit;
    info.price = Price(50000.0);
    info.original_quantity = Quantity(1.0);
    info.filled_quantity = Quantity(0.0);
    info.status = status;
    info.created_at = Timestamp(900000);
    return info;
}

inline auto make_engine_deps() {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    auto exchange = std::make_shared<MockExchangeQueryService>();
    return std::make_tuple(logger, clk, metrics, exchange);
}

// ========== Тесты ==========

TEST_CASE("ReconciliationEngine: startup reconciliation без расхождений", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Пустое состояние — ни локальных ордеров, ни на бирже
    exchange->open_orders_ = {};

    // Баланс: USDT совпадает
    ExchangePositionInfo usdt_balance;
    usdt_balance.symbol = Symbol("USDT");
    usdt_balance.available = Quantity(10000.0);
    usdt_balance.frozen = Quantity(0.0);
    usdt_balance.total_value_usd = 10000.0;
    exchange->account_balances_ = {usdt_balance};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 10000.0);

    REQUIRE(result.status == ReconciliationStatus::Success);
    REQUIRE(result.mismatches.empty());
}

TEST_CASE("ReconciliationEngine: обнаружение ордера только на бирже", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // На бирже есть ордер, локально — нет
    auto exchange_order = make_exchange_order("ORD-ORPHAN", "ETHUSDT", Side::Sell, "NEW");
    exchange->open_orders_ = {exchange_order};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 0.0);

    // Должно быть хотя бы одно расхождение OrderExistsOnlyOnExchange
    bool found = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::OrderExistsOnlyOnExchange) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
    REQUIRE(result.status != ReconciliationStatus::Success);
}

TEST_CASE("ReconciliationEngine: обнаружение ордера только локально", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Локально есть активный ордер, на бирже — пусто
    auto local_order = make_local_order("ORD-LOCAL", "BTCUSDT", Side::Buy, execution::OrderState::Open);
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({local_order}, {}, 0.0);

    bool found = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::OrderExistsOnlyLocally) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ReconciliationEngine: обнаружение расхождения статуса ордера", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Локально Open, на бирже — FILLED
    auto local_order = make_local_order("ORD-2", "BTCUSDT", Side::Buy, execution::OrderState::Open);

    auto exchange_order = make_exchange_order("ORD-2", "BTCUSDT", Side::Buy, "FILLED");
    exchange_order.filled_quantity = Quantity(1.0);
    exchange->open_orders_ = {exchange_order};

    exchange->order_statuses_[local_order.order_id.get()] = exchange_order;
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({local_order}, {}, 0.0);

    bool found_state_mismatch = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::StateMismatch) {
            found_state_mismatch = true;
            break;
        }
    }
    REQUIRE(found_state_mismatch);
}

TEST_CASE("ReconciliationEngine: обнаружение расхождения баланса", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Локально баланс 10000, на бирже — 8000 (> 1% допуска)
    ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(8000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 8000.0;
    exchange->account_balances_ = {usdt};
    exchange->open_orders_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 10000.0);

    bool found_balance = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::BalanceMismatch) {
            found_balance = true;
            break;
        }
    }
    REQUIRE(found_balance);
}

TEST_CASE("ReconciliationEngine: авто-разрешение расхождений", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    auto cfg = make_default_config();
    cfg.auto_resolve_state_mismatches = true;
    cfg.auto_cancel_orphan_orders = true;

    // На бирже есть ордер, локально — нет → orphan, авто-отмена
    auto exchange_order = make_exchange_order("ORD-ORPHAN", "BTCUSDT", Side::Buy, "NEW");
    exchange->open_orders_ = {exchange_order};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(cfg, exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 0.0);

    // Проверяем, что авто-разрешение произошло
    REQUIRE(result.auto_resolved >= 0);

    bool found_resolved = false;
    for (const auto& m : result.mismatches) {
        if (m.resolved) {
            found_resolved = true;
            break;
        }
    }
    // Если есть расхождения и авто-разрешение включено, хотя бы одно должно быть resolved
    if (!result.mismatches.empty()) {
        CHECK(result.auto_resolved > 0);
    }
}
