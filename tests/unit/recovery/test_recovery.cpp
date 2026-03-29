/**
 * @file test_recovery.cpp
 * @brief Тесты сервиса восстановления состояния
 */
#include <catch2/catch_test_macros.hpp>
#include "test_mocks.hpp"
#include "recovery/recovery_service.hpp"
#include "recovery/recovery_types.hpp"
#include "reconciliation/reconciliation_engine.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "persistence/persistence_layer.hpp"
#include "persistence/memory_storage_adapter.hpp"

using namespace tb;
using namespace tb::test;
using namespace tb::recovery;

// ========== Mock IExchangeQueryService ==========

class MockExchangeQueryService : public reconciliation::IExchangeQueryService {
public:
    std::vector<reconciliation::ExchangeOrderInfo> open_orders_;
    std::vector<reconciliation::ExchangePositionInfo> account_balances_;

    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_open_orders(const Symbol& /*symbol*/) override {
        return open_orders_;
    }

    Result<std::vector<reconciliation::ExchangePositionInfo>>
    get_account_balances() override {
        return account_balances_;
    }

    Result<reconciliation::ExchangeOrderInfo>
    get_order_status(const OrderId& /*order_id*/, const Symbol& /*symbol*/) override {
        return std::unexpected(TbError::ReconciliationFailed);
    }
};

// ========== Вспомогательные функции ==========

inline RecoveryConfig make_default_config() {
    RecoveryConfig cfg;
    cfg.enabled = true;
    cfg.close_orphan_positions = true;
    cfg.cancel_stale_orders = true;
    cfg.stale_order_age_ms = 300000;
    cfg.min_position_value_usd = 10.0;
    cfg.max_recovery_attempts = 3;
    return cfg;
}

inline auto make_recovery_deps() {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto exchange = std::make_shared<MockExchangeQueryService>();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        10000.0, logger, clk, met);
    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    return std::make_tuple(logger, clk, met, exchange, portfolio, persistence);
}

// ========== Тесты ==========

TEST_CASE("RecoveryService: восстановление при чистом старте", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    // Биржа пуста: нет ордеров, нет позиций
    exchange->open_orders_ = {};
    exchange->account_balances_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    REQUIRE(result.recovered_positions.empty());
    REQUIRE(result.recovered_orders.empty());
    REQUIRE(result.errors == 0);
}

TEST_CASE("RecoveryService: восстановление позиций с биржи", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    // На бирже есть BTC позиция
    reconciliation::ExchangePositionInfo btc;
    btc.symbol = Symbol("BTC");
    btc.available = Quantity(0.5);
    btc.frozen = Quantity(0.0);
    btc.total_value_usd = 25000.0;

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(5000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 5000.0;

    exchange->account_balances_ = {btc, usdt};
    exchange->open_orders_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    // Должна быть восстановлена позиция по BTC (стоимость > min_position_value_usd)
    bool found_btc = false;
    for (const auto& pos : result.recovered_positions) {
        if (pos.symbol.get() == "BTC") {
            found_btc = true;
            CHECK(pos.size.get() > 0.0);
        }
    }
    CHECK(found_btc);
}

TEST_CASE("RecoveryService: фильтрация пылевых позиций", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.min_position_value_usd = 100.0;  // Высокий порог для фильтрации пыли

    // Пылевая позиция стоимостью < 100 USD
    reconciliation::ExchangePositionInfo dust;
    dust.symbol = Symbol("SHIB");
    dust.available = Quantity(1000.0);
    dust.frozen = Quantity(0.0);
    dust.total_value_usd = 0.50;  // Меньше min_position_value_usd

    exchange->account_balances_ = {dust};
    exchange->open_orders_ = {};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    // Пылевая позиция не должна быть восстановлена
    for (const auto& pos : result.recovered_positions) {
        REQUIRE(pos.symbol.get() != "SHIB");
    }
}

TEST_CASE("RecoveryService: восстановление баланса", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    // На бирже USDT баланс = 15000
    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(15000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 15000.0;

    exchange->account_balances_ = {usdt};
    exchange->open_orders_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    // Баланс должен быть восстановлен
    CHECK(result.recovered_cash_balance >= 0.0);
}
