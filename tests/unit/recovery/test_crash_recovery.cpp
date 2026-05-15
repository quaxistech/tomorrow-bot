/**
 * @file test_crash_recovery.cpp
 * @brief Deterministic crash-recovery harness (Phase 1.3).
 *
 * Симулирует:
 *  1. Pre-crash state: открытая фьючерсная позиция, реализованный PnL, синхронизированный капитал.
 *  2. Persist: snapshot портфеля сохраняется в `MemoryStorageAdapter` через `pipeline-format` payload.
 *  3. "SIGKILL": уничтожение portfolio_engine + recovery_service объектов.
 *  4. Restart: новые объекты, пустой портфель, тот же storage adapter.
 *  5. Recovery: `RecoveryService::recover_from_journal()` восстанавливает state.
 *  6. Verification: post-recovery state == pre-crash state (positions, capital).
 *
 * Этот harness НЕ заменяет полный integration-test против реальной биржи
 * (где snapshot+exchange truth комбинируются), но даёт детерминированный test для
 * code-path "snapshot persist → restore" — ключевой инвариант для recovery после kill -9.
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

#include <boost/json.hpp>
#include <memory>
#include <string>

using namespace tb;
using namespace tb::test;

namespace {

class NullExchangeQueryService : public reconciliation::IExchangeQueryService {
public:
    Result<std::vector<reconciliation::ExchangeOrderInfo>>
    get_open_orders(const Symbol& /*symbol*/) override {
        return std::vector<reconciliation::ExchangeOrderInfo>{};
    }

    Result<std::vector<reconciliation::ExchangePositionInfo>>
    get_account_balances() override {
        return std::vector<reconciliation::ExchangePositionInfo>{};
    }

    Result<std::vector<reconciliation::ExchangeOpenPositionInfo>>
    get_open_positions(const Symbol& /*symbol*/) override {
        return std::vector<reconciliation::ExchangeOpenPositionInfo>{};
    }

    Result<reconciliation::ExchangeOrderInfo>
    get_order_status(const OrderId& /*order_id*/, const Symbol& /*symbol*/) override {
        return std::unexpected(TbError::ReconciliationFailed);
    }
};

/// Сериализует портфельный снимок в формате, совместимом с `RecoveryService::restore_from_snapshot`.
/// Должен соответствовать `TradingPipeline::persist_portfolio_snapshot()`.
std::string serialize_portfolio_snapshot(const portfolio::PortfolioSnapshot& snap,
                                         const Symbol& symbol,
                                         std::int64_t now_ns) {
    boost::json::array positions_arr;
    for (const auto& pos : snap.positions) {
        boost::json::object pobj;
        pobj["symbol"] = pos.symbol.get();
        pobj["side"] = (pos.side == Side::Buy ? "Buy" : "Sell");
        pobj["position_side"] = (pos.position_side == PositionSide::Long ? "long" : "short");
        pobj["size"] = pos.size.get();
        pobj["avg_entry_price"] = pos.avg_entry_price.get();
        pobj["current_price"] = pos.current_price.get();
        pobj["unrealized_pnl"] = pos.unrealized_pnl;
        pobj["strategy_id"] = pos.strategy_id.get();
        pobj["opened_at_ns"] = pos.opened_at.get();
        pobj["updated_at_ns"] = pos.updated_at.get();
        positions_arr.push_back(std::move(pobj));
    }

    boost::json::object root;
    root["symbol"] = symbol.get();
    root["positions"] = std::move(positions_arr);
    root["total_capital"] = snap.total_capital;
    root["available_capital"] = snap.available_capital;
    root["timestamp_ns"] = now_ns;

    return boost::json::serialize(root);
}

recovery::RecoveryConfig make_config() {
    recovery::RecoveryConfig cfg;
    cfg.enabled = true;
    cfg.close_orphan_positions = false;
    cfg.min_position_value_usd = 1.0;
    return cfg;
}

} // anonymous namespace

// ============================================================
// Sub-test 1: snapshot save → SIGKILL → restore
// ============================================================
TEST_CASE("CrashRecovery: открытая позиция переживает SIGKILL+restart", "[recovery][crash]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    // ── Pre-crash: реалистичное состояние портфеля ──
    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();

    {
        auto pre_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
            10000.0, logger, clk, met);
        pre_portfolio->set_capital(9876.54);

        portfolio::Position pos;
        pos.symbol = Symbol("BTCUSDT");
        pos.side = Side::Buy;
        pos.position_side = PositionSide::Long;
        pos.size = Quantity(0.025);
        pos.avg_entry_price = Price(63'500.0);
        pos.current_price = Price(63'500.0);
        pos.notional = NotionalValue(63'500.0 * 0.025);
        pos.strategy_id = StrategyId("scalp_v1");
        pos.opened_at = Timestamp(clk->current_time);
        pos.updated_at = Timestamp(clk->current_time);
        pre_portfolio->open_position(pos);

        // Persist через тот же adapter — точно тот же путь, что и в production pipeline.
        auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
        const auto pre_snap = pre_portfolio->snapshot();
        const auto payload = serialize_portfolio_snapshot(pre_snap, Symbol("BTCUSDT"), clk->current_time);
        auto save_result = persistence->snapshots().save(persistence::SnapshotType::Portfolio, payload);
        REQUIRE(save_result.has_value());

        // Симуляция "SIGKILL": все объекты выходят из scope без stop().
        // adapter переживает (это shared_ptr, держим вне scope) — это эквивалент
        // PostgreSQL: данные на диске между процессами.
    }

    // ── Post-crash: новый процесс, пустой портфель, тот же adapter ──
    auto post_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        0.0, logger, clk, met);  // capital=0 — будет восстановлен из snapshot
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    auto exchange = std::make_shared<NullExchangeQueryService>();

    recovery::RecoveryService service(make_config(), exchange, post_portfolio,
                                       persistence, logger, clk, met);

    auto result = service.recover_from_journal();

    // ── Verification: state восстановлен? ──
    REQUIRE(result.status != recovery::RecoveryStatus::Failed);

    auto post_snap = post_portfolio->snapshot();
    REQUIRE(post_snap.total_capital == Catch::Approx(9876.54));
    REQUIRE(post_snap.positions.size() == 1);

    const auto& restored = post_snap.positions.front();
    REQUIRE(restored.symbol.get() == "BTCUSDT");
    REQUIRE(restored.side == Side::Buy);
    REQUIRE(restored.size.get() == Catch::Approx(0.025));
    REQUIRE(restored.avg_entry_price.get() == Catch::Approx(63'500.0));
    REQUIRE(restored.strategy_id.get() == "scalp_v1");
}

// ============================================================
// Sub-test 2: empty pre-crash → recover → empty
// ============================================================
TEST_CASE("CrashRecovery: пустой портфель остаётся пустым после recover", "[recovery][crash]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();

    {
        auto pre_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
            10000.0, logger, clk, met);
        auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
        const auto pre_snap = pre_portfolio->snapshot();
        const auto payload = serialize_portfolio_snapshot(pre_snap, Symbol("BTCUSDT"), clk->current_time);
        REQUIRE(persistence->snapshots().save(persistence::SnapshotType::Portfolio, payload).has_value());
    }

    auto post_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        0.0, logger, clk, met);
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    auto exchange = std::make_shared<NullExchangeQueryService>();

    recovery::RecoveryService service(make_config(), exchange, post_portfolio,
                                       persistence, logger, clk, met);
    auto result = service.recover_from_journal();
    REQUIRE(result.status != recovery::RecoveryStatus::Failed);

    REQUIRE(post_portfolio->snapshot().positions.empty());
    REQUIRE(post_portfolio->snapshot().total_capital == Catch::Approx(10000.0));
}

// ============================================================
// Sub-test 3: пустой adapter (нет snapshot) — recover не падает
// ============================================================
TEST_CASE("CrashRecovery: отсутствие snapshot не вызывает Failed", "[recovery][crash]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();
    auto post_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        5000.0, logger, clk, met);
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    auto exchange = std::make_shared<NullExchangeQueryService>();

    recovery::RecoveryService service(make_config(), exchange, post_portfolio,
                                       persistence, logger, clk, met);
    auto result = service.recover_from_journal();
    // Recovery без snapshot должна возвращать Success с empty state, не Failed.
    REQUIRE(result.status != recovery::RecoveryStatus::Failed);

    // Капитал и позиции остаются как при construction (нет snapshot для перезаписи).
    auto post_snap = post_portfolio->snapshot();
    REQUIRE(post_snap.total_capital == Catch::Approx(5000.0));
    REQUIRE(post_snap.positions.empty());
}

// ============================================================
// Sub-test 4: Long+Short hedge-mode позиции переживают crash
// ============================================================
TEST_CASE("CrashRecovery: hedge-mode (Long+Short одновременно) переживает SIGKILL", "[recovery][crash][hedge]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();

    {
        auto pre_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
            10000.0, logger, clk, met);

        // Long leg
        portfolio::Position long_pos;
        long_pos.symbol = Symbol("ETHUSDT");
        long_pos.side = Side::Buy;
        long_pos.position_side = PositionSide::Long;
        long_pos.size = Quantity(0.5);
        long_pos.avg_entry_price = Price(3'200.0);
        long_pos.current_price = Price(3'200.0);
        long_pos.strategy_id = StrategyId("scalp");
        long_pos.opened_at = Timestamp(clk->current_time);
        long_pos.updated_at = Timestamp(clk->current_time);
        pre_portfolio->open_position(long_pos);

        // Short leg (hedge)
        portfolio::Position short_pos;
        short_pos.symbol = Symbol("ETHUSDT");
        short_pos.side = Side::Sell;
        short_pos.position_side = PositionSide::Short;
        short_pos.size = Quantity(0.5);
        short_pos.avg_entry_price = Price(3'205.0);
        short_pos.current_price = Price(3'205.0);
        short_pos.strategy_id = StrategyId("scalp");
        short_pos.opened_at = Timestamp(clk->current_time);
        short_pos.updated_at = Timestamp(clk->current_time);
        pre_portfolio->open_position(short_pos);

        REQUIRE(pre_portfolio->snapshot().positions.size() == 2);

        auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
        const auto pre_snap = pre_portfolio->snapshot();
        const auto payload = serialize_portfolio_snapshot(pre_snap, Symbol("ETHUSDT"), clk->current_time);
        REQUIRE(persistence->snapshots().save(persistence::SnapshotType::Portfolio, payload).has_value());
    }

    auto post_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        0.0, logger, clk, met);
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    auto exchange = std::make_shared<NullExchangeQueryService>();

    recovery::RecoveryService service(make_config(), exchange, post_portfolio,
                                       persistence, logger, clk, met);
    auto result = service.recover_from_journal();
    REQUIRE(result.status != recovery::RecoveryStatus::Failed);

    auto post_snap = post_portfolio->snapshot();
    REQUIRE(post_snap.positions.size() == 2);

    // Verify both legs are present (hedge mode invariant).
    bool has_long = false, has_short = false;
    for (const auto& p : post_snap.positions) {
        if (p.side == Side::Buy) {
            has_long = true;
            REQUIRE(p.size.get() == Catch::Approx(0.5));
            REQUIRE(p.avg_entry_price.get() == Catch::Approx(3'200.0));
        } else {
            has_short = true;
            REQUIRE(p.size.get() == Catch::Approx(0.5));
            REQUIRE(p.avg_entry_price.get() == Catch::Approx(3'205.0));
        }
    }
    REQUIRE(has_long);
    REQUIRE(has_short);
}

// ============================================================
// Sub-test 5: invariant — capital_synced=true после recovery → reset_daily не сбрасывает капитал
// ============================================================
TEST_CASE("CrashRecovery: post-recovery reset_daily не теряет капитал", "[recovery][crash][invariant]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    auto adapter = std::make_shared<persistence::MemoryStorageAdapter>();

    {
        auto pre_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
            10000.0, logger, clk, met);
        pre_portfolio->set_capital(7777.77);

        auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
        const auto pre_snap = pre_portfolio->snapshot();
        const auto payload = serialize_portfolio_snapshot(pre_snap, Symbol("BTCUSDT"), clk->current_time);
        REQUIRE(persistence->snapshots().save(persistence::SnapshotType::Portfolio, payload).has_value());
    }

    auto post_portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        0.0, logger, clk, met);
    auto persistence = std::make_shared<persistence::PersistenceLayer>(adapter);
    auto exchange = std::make_shared<NullExchangeQueryService>();

    recovery::RecoveryService service(make_config(), exchange, post_portfolio,
                                       persistence, logger, clk, met);
    auto result = service.recover_from_journal();
    REQUIRE(result.status != recovery::RecoveryStatus::Failed);

    REQUIRE(post_portfolio->snapshot().total_capital == Catch::Approx(7777.77));

    // Daily reset — должен сбросить дневные счётчики, но капитал/позиции сохранить.
    post_portfolio->reset_daily();
    REQUIRE(post_portfolio->snapshot().total_capital == Catch::Approx(7777.77));
}
