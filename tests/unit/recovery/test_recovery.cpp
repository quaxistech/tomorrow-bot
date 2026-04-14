/**
 * @file test_recovery.cpp
 * @brief Тесты сервиса восстановления состояния USDT-M Futures
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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
    std::vector<reconciliation::ExchangeOpenPositionInfo> open_positions_;

    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_open_orders(const Symbol& /*symbol*/) override {
        return open_orders_;
    }

    Result<std::vector<reconciliation::ExchangePositionInfo>>
    get_account_balances() override {
        return account_balances_;
    }

    Result<std::vector<reconciliation::ExchangeOpenPositionInfo>>
    get_open_positions(const Symbol& symbol) override {
        if (symbol.get().empty()) {
            return open_positions_;
        }

        std::vector<reconciliation::ExchangeOpenPositionInfo> filtered;
        for (const auto& pos : open_positions_) {
            if (pos.symbol.get() == symbol.get()) {
                filtered.push_back(pos);
            }
        }
        return filtered;
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
    cfg.close_orphan_positions = false;
    cfg.min_position_value_usd = 5.0;
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

    exchange->open_orders_ = {};
    exchange->account_balances_ = {};
    exchange->open_positions_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    REQUIRE(result.recovered_positions.empty());
    REQUIRE(result.errors == 0);
}

TEST_CASE("RecoveryService: восстановление фьючерсных позиций с биржи", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.close_orphan_positions = false;

    // На бирже есть BTCUSDT Long позиция
    reconciliation::ExchangeOpenPositionInfo btc_long;
    btc_long.symbol = Symbol("BTCUSDT");
    btc_long.side = Side::Buy;
    btc_long.size = Quantity(0.1);
    btc_long.entry_price = Price(65000.0);
    btc_long.current_price = Price(65500.0);
    btc_long.notional_usd = 6550.0;
    btc_long.unrealized_pnl = 50.0;

    exchange->open_positions_ = {btc_long};

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(5000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 5000.0;

    exchange->account_balances_ = {usdt};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    REQUIRE(result.recovered_positions.size() == 1);
    CHECK(result.recovered_positions.front().symbol.get() == "BTCUSDT");
    CHECK(result.recovered_positions.front().side == Side::Buy);
    CHECK(result.recovered_positions.front().size.get() == Catch::Approx(0.1));

    auto restored = portfolio->get_position(Symbol("BTCUSDT"));
    REQUIRE(restored.has_value());
    CHECK(restored->side == Side::Buy);
}

TEST_CASE("RecoveryService: фильтрация пылевых фьючерсных позиций", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.min_position_value_usd = 100.0;

    // Пылевая позиция: notional < min_position_value_usd
    reconciliation::ExchangeOpenPositionInfo dust;
    dust.symbol = Symbol("DOGEUSDT");
    dust.side = Side::Buy;
    dust.size = Quantity(10.0);
    dust.entry_price = Price(0.08);
    dust.current_price = Price(0.08);
    dust.notional_usd = 0.80;  // < 100 USD
    dust.unrealized_pnl = 0.0;

    exchange->open_positions_ = {dust};

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(10000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 10000.0;
    exchange->account_balances_ = {usdt};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    REQUIRE(result.recovered_positions.empty());
}

TEST_CASE("RecoveryService: восстановление маржевого баланса", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    // На бирже USDT маржевый баланс = 15000
    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(15000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 15000.0;

    exchange->account_balances_ = {usdt};
    exchange->open_positions_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    CHECK(result.recovered_cash_balance == Catch::Approx(15000.0));

    auto snap = portfolio->snapshot();
    CHECK(snap.total_capital == Catch::Approx(15000.0));
}

TEST_CASE("RecoveryService: futures позиции Long и Short с направлением", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.symbol_filter = Symbol("ETHUSDT");
    cfg.close_orphan_positions = false;

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(3200.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 3200.0;
    exchange->account_balances_ = {usdt};

    reconciliation::ExchangeOpenPositionInfo eth_short;
    eth_short.symbol = Symbol("ETHUSDT");
    eth_short.side = Side::Sell;
    eth_short.size = Quantity(0.75);
    eth_short.entry_price = Price(3200.0);
    eth_short.current_price = Price(3150.0);
    eth_short.notional_usd = 2362.5;
    eth_short.unrealized_pnl = 37.5;

    reconciliation::ExchangeOpenPositionInfo btc_long;
    btc_long.symbol = Symbol("BTCUSDT");
    btc_long.side = Side::Buy;
    btc_long.size = Quantity(0.1);
    btc_long.entry_price = Price(65000.0);
    btc_long.current_price = Price(65200.0);
    btc_long.notional_usd = 6520.0;
    btc_long.unrealized_pnl = 20.0;

    exchange->open_positions_ = {eth_short, btc_long};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);
    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    // Только ETHUSDT должна пройти (symbol_filter)
    REQUIRE(result.recovered_positions.size() == 1);
    CHECK(result.recovered_positions.front().symbol.get() == "ETHUSDT");
    CHECK(result.recovered_positions.front().side == Side::Sell);

    auto restored = portfolio->get_position(Symbol("ETHUSDT"));
    REQUIRE(restored.has_value());
    CHECK(restored->side == Side::Sell);
    CHECK(restored->size.get() == Catch::Approx(0.75));
    CHECK_FALSE(portfolio->has_position(Symbol("BTCUSDT")));
}

TEST_CASE("RecoveryService: disabled recovery exits cleanly", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.enabled = false;

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);
    auto result = service.recover_on_startup();

    CHECK(result.status == RecoveryStatus::Completed);
    CHECK(result.errors == 0);
    CHECK(result.recovered_positions.empty());
}

TEST_CASE("RecoveryService: close_orphan_positions=true пропускает orphan", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.close_orphan_positions = true;

    reconciliation::ExchangeOpenPositionInfo orphan;
    orphan.symbol = Symbol("SOLUSDT");
    orphan.side = Side::Buy;
    orphan.size = Quantity(5.0);
    orphan.entry_price = Price(150.0);
    orphan.current_price = Price(152.0);
    orphan.notional_usd = 760.0;
    orphan.unrealized_pnl = 10.0;

    exchange->open_positions_ = {orphan};

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(5000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 5000.0;
    exchange->account_balances_ = {usdt};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);
    auto result = service.recover_on_startup();

    REQUIRE(result.status != RecoveryStatus::Failed);
    REQUIRE(result.recovered_positions.size() == 1);
    // Позиция должна быть зафиксирована, но НЕ синхронизирована в портфель
    CHECK_FALSE(result.recovered_positions.front().had_matching_strategy);
    CHECK_FALSE(portfolio->has_position(Symbol("SOLUSDT")));
    CHECK(result.warnings > 0);
}

TEST_CASE("RecoveryService: quantity mismatch при расхождении > 0.5%", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    auto cfg = make_default_config();
    cfg.close_orphan_positions = false;

    // Сначала открываем позицию в портфеле
    portfolio::Position local_pos;
    local_pos.symbol = Symbol("BTCUSDT");
    local_pos.side = Side::Buy;
    local_pos.size = Quantity(0.100);
    local_pos.avg_entry_price = Price(65000.0);
    local_pos.current_price = Price(65000.0);
    local_pos.notional = NotionalValue(6500.0);
    local_pos.strategy_id = StrategyId("scalper");
    portfolio->open_position(local_pos);

    // На бирже позиция с заметно другим количеством (2% разница)
    reconciliation::ExchangeOpenPositionInfo btc;
    btc.symbol = Symbol("BTCUSDT");
    btc.side = Side::Buy;
    btc.size = Quantity(0.102);  // 2% больше, чем 0.100
    btc.entry_price = Price(65000.0);
    btc.current_price = Price(65000.0);
    btc.notional_usd = 6630.0;
    btc.unrealized_pnl = 0.0;

    exchange->open_positions_ = {btc};

    reconciliation::ExchangePositionInfo usdt;
    usdt.symbol = Symbol("USDT");
    usdt.available = Quantity(10000.0);
    usdt.frozen = Quantity(0.0);
    usdt.total_value_usd = 10000.0;
    exchange->account_balances_ = {usdt};

    RecoveryService service(cfg, exchange, portfolio, persistence, logger, clk, met);
    auto result = service.recover_on_startup();

    REQUIRE(result.status == RecoveryStatus::CompletedWithWarnings);
    REQUIRE(result.recovered_positions.size() == 1);
    CHECK(result.recovered_positions.front().had_matching_strategy);
    CHECK(result.warnings > 0);
    // Resolution должен содержать "mismatch"
    CHECK(result.recovered_positions.front().resolution.find("mismatch") != std::string::npos);
}

TEST_CASE("RecoveryService: last_result возвращает копию (потокобезопасность)", "[recovery]") {
    auto [logger, clk, met, exchange, portfolio, persistence] = make_recovery_deps();

    exchange->account_balances_ = {};
    exchange->open_positions_ = {};

    RecoveryService service(make_default_config(), exchange, portfolio,
                            persistence, logger, clk, met);

    service.recover_on_startup();

    // last_result() возвращает копию — должна быть валидна
    auto result_copy = service.last_result();
    CHECK(result_copy.status != RecoveryStatus::NotStarted);
    CHECK(result_copy.status != RecoveryStatus::InProgress);
}
