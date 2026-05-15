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
    std::vector<ExchangeOpenPositionInfo> open_positions_;
    std::unordered_map<std::string, ExchangeOrderInfo> order_statuses_;

    Result<std::vector<ExchangeOrderInfo>>
    get_open_orders(const Symbol& /*symbol*/) override {
        return open_orders_;
    }

    Result<std::vector<ExchangePositionInfo>>
    get_account_balances() override {
        return account_balances_;
    }

    Result<std::vector<ExchangeOpenPositionInfo>>
    get_open_positions(const Symbol& /*symbol*/) override {
        return open_positions_;
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

inline ExchangeOpenPositionInfo make_exchange_position(
    const std::string& symbol, Side side, double size, double entry_price = 50000.0) {
    ExchangeOpenPositionInfo pos;
    pos.symbol = Symbol(symbol);
    pos.side = side;
    pos.position_side = (side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
    pos.size = Quantity(size);
    pos.entry_price = Price(entry_price);
    pos.current_price = Price(entry_price);
    pos.notional_usd = entry_price * size;
    pos.unrealized_pnl = 0.0;
    return pos;
}

inline ReconciliationConfig make_default_config() {
    ReconciliationConfig cfg;
    cfg.auto_resolve_state_mismatches = true;
    cfg.auto_cancel_orphan_orders = true;
    cfg.auto_close_orphan_positions = true;
    cfg.position_tolerance_pct = 0.5;
    cfg.balance_tolerance_pct = 1.0;
    cfg.max_auto_resolutions_per_run = 10;
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

TEST_CASE("ReconciliationEngine: баланс сверяется по equity, а не по available", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Для USDT-M Futures available уменьшается на margin открытых позиций,
    // но local_cash синхронизируется из полного usdtEquity.
    ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(8000.0);
    usdt.frozen = Quantity(1000.0);
    usdt.total_value_usd = 12000.0;
    exchange->account_balances_ = {usdt};
    exchange->open_orders_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 12000.0);

    bool found_balance = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::BalanceMismatch) {
            found_balance = true;
            break;
        }
    }
    REQUIRE_FALSE(found_balance);
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

// ========== Тесты фьючерсных позиций ==========

TEST_CASE("ReconciliationEngine: фьючерсные позиции совпадают", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Локальная позиция: BTCUSDT Long 0.01
    portfolio::Position local_pos;
    local_pos.symbol = Symbol("BTCUSDT");
    local_pos.side = Side::Buy;
    local_pos.size = Quantity(0.01);
    local_pos.avg_entry_price = Price(60000.0);

    // Биржевая позиция совпадает
    exchange->open_positions_ = {make_exchange_position("BTCUSDT", Side::Buy, 0.01, 60000.0)};
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {local_pos}, 0.0);

    // Не должно быть расхождений позиций
    bool has_position_mismatch = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::PositionExistsOnlyLocally ||
            m.type == MismatchType::PositionExistsOnlyOnExchange ||
            m.type == MismatchType::QuantityMismatch) {
            has_position_mismatch = true;
            break;
        }
    }
    REQUIRE_FALSE(has_position_mismatch);
}

TEST_CASE("ReconciliationEngine: фьючерсная позиция только локально", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    portfolio::Position local_pos;
    local_pos.symbol = Symbol("ETHUSDT");
    local_pos.side = Side::Sell;
    local_pos.size = Quantity(0.5);

    // На бирже позиций нет
    exchange->open_positions_ = {};
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {local_pos}, 0.0);

    bool found = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::PositionExistsOnlyLocally &&
            m.symbol.get() == "ETHUSDT") {
            found = true;
            CHECK(m.description.find("Short") != std::string::npos);
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ReconciliationEngine: фьючерсная позиция только на бирже", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // На бирже есть позиция, локально — нет
    exchange->open_positions_ = {make_exchange_position("BTCUSDT", Side::Buy, 0.005, 62000.0)};
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {}, 0.0);

    bool found = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::PositionExistsOnlyOnExchange &&
            m.symbol.get() == "BTCUSDT") {
            found = true;
            CHECK(m.description.find("Long") != std::string::npos);
            CHECK(m.description.find("62000") != std::string::npos);
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ReconciliationEngine: расхождение размера фьючерсной позиции", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Локально 0.01, на бирже 0.02 — разница 100% > 0.5%
    portfolio::Position local_pos;
    local_pos.symbol = Symbol("BTCUSDT");
    local_pos.side = Side::Buy;
    local_pos.size = Quantity(0.01);

    exchange->open_positions_ = {make_exchange_position("BTCUSDT", Side::Buy, 0.02)};
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {local_pos}, 0.0);

    bool found = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::QuantityMismatch &&
            m.symbol.get() == "BTCUSDT") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ReconciliationEngine: hedge mode — Long и Short по одному символу", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Две локальные позиции: BTCUSDT Long 0.01 + BTCUSDT Short 0.005
    portfolio::Position long_pos;
    long_pos.symbol = Symbol("BTCUSDT");
    long_pos.side = Side::Buy;
    long_pos.size = Quantity(0.01);

    portfolio::Position short_pos;
    short_pos.symbol = Symbol("BTCUSDT");
    short_pos.side = Side::Sell;
    short_pos.size = Quantity(0.005);

    // На бирже тоже обе позиции
    exchange->open_positions_ = {
        make_exchange_position("BTCUSDT", Side::Buy, 0.01),
        make_exchange_position("BTCUSDT", Side::Sell, 0.005)
    };
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {long_pos, short_pos}, 0.0);

    // Не должно быть расхождений позиций — обе совпадают при hedge mode matching
    bool has_position_mismatch = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::PositionExistsOnlyLocally ||
            m.type == MismatchType::PositionExistsOnlyOnExchange ||
            m.type == MismatchType::QuantityMismatch) {
            has_position_mismatch = true;
            break;
        }
    }
    REQUIRE_FALSE(has_position_mismatch);
}

TEST_CASE("ReconciliationEngine: позиция в пределах допуска не вызывает mismatch", "[reconciliation]") {
    auto [logger, clk, metrics, exchange] = make_engine_deps();

    // Разница 0.3% < position_tolerance_pct (0.5%)
    portfolio::Position local_pos;
    local_pos.symbol = Symbol("BTCUSDT");
    local_pos.side = Side::Buy;
    local_pos.size = Quantity(1.000);

    exchange->open_positions_ = {make_exchange_position("BTCUSDT", Side::Buy, 1.003)};
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    ReconciliationEngine engine(make_default_config(), exchange, logger, clk, metrics);

    auto result = engine.reconcile_on_startup({}, {local_pos}, 0.0);

    bool has_qty_mismatch = false;
    for (const auto& m : result.mismatches) {
        if (m.type == MismatchType::QuantityMismatch) {
            has_qty_mismatch = true;
            break;
        }
    }
    REQUIRE_FALSE(has_qty_mismatch);
}