#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_mocks.hpp"
#include "portfolio_allocator/portfolio_allocator.hpp"
#include "portfolio/portfolio_types.hpp"
#include "strategy/strategy_types.hpp"

using namespace tb;
using namespace tb::test;
using namespace tb::portfolio_allocator;
using namespace Catch::Matchers;

// ========== Вспомогательные функции ==========

static strategy::TradeIntent make_intent(
    const std::string& symbol = "BTCUSDT",
    double qty = 0.1, double price = 50000.0,
    const std::string& strategy = "test_strategy")
{
    strategy::TradeIntent intent;
    intent.symbol = Symbol(symbol);
    intent.side = Side::Buy;
    intent.signal_intent = strategy::SignalIntent::LongEntry;
    intent.suggested_quantity = Quantity(qty);
    intent.limit_price = Price(price);
    intent.strategy_id = StrategyId(strategy);
    intent.conviction = 0.8;
    return intent;
}

static portfolio::PortfolioSnapshot make_snapshot(double capital = 10000.0,
                                                   double available = 10000.0) {
    portfolio::PortfolioSnapshot snap;
    snap.total_capital = capital;
    snap.available_capital = available;
    return snap;
}

static AllocationContext make_context(double vol = 0.3,
                                      regime::DetailedRegime regime = regime::DetailedRegime::StrongUptrend) {
    AllocationContext ctx;
    ctx.realized_vol_annual = vol;
    ctx.regime = regime;
    ctx.win_rate = 0.55;
    ctx.avg_win_loss_ratio = 1.5;
    // Default exchange filters для тестов
    ExchangeFilters ef;
    ef.min_quantity = 0.00001;
    ef.quantity_step = 0.00001;
    ef.min_notional = 1.0;
    ef.max_quantity = 1000.0;
    ef.taker_fee_pct = 0.0006;
    ctx.exchange_filters = ef;
    return ctx;
}

// ==================== Legacy API (compute_size) ====================

TEST_CASE("Allocator: compute_size одобряет нормальный ордер", "[allocator]") {
    HierarchicalAllocator::Config cfg;
    cfg.budget.global_budget = 10000.0;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);
    auto snap = make_snapshot();

    auto result = allocator.compute_size(intent, snap, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.approved_quantity.get() > 0.0);
}

TEST_CASE("Allocator: compute_size отклоняет без цены", "[allocator]") {
    HierarchicalAllocator::Config cfg;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    strategy::TradeIntent intent;
    intent.symbol = Symbol("BTCUSDT");
    intent.suggested_quantity = Quantity(0.1);
    // No limit_price

    auto result = allocator.compute_size(intent, make_snapshot(), 1.0);
    REQUIRE_FALSE(result.approved);
}

TEST_CASE("Allocator: compute_size применяет бюджетный лимит", "[allocator]") {
    HierarchicalAllocator::Config cfg;
    cfg.budget.symbol_budget_pct = 0.05;  // 5% per symbol = $500
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    // Trying to buy $5000 worth
    auto intent = make_intent("BTCUSDT", 0.1, 50000.0);
    auto snap = make_snapshot();

    auto result = allocator.compute_size(intent, snap, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.was_reduced);
    REQUIRE(result.approved_notional.get() <= 500.01);
}

TEST_CASE("Allocator: compute_size применяет лимит концентрации", "[allocator]") {
    HierarchicalAllocator::Config cfg;
    cfg.max_concentration_pct = 0.10;  // 10%
    cfg.budget.symbol_budget_pct = 0.50; // High budget so concentration is binding
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 1.0, 5000.0);  // $5000 notional
    auto snap = make_snapshot();  // $10000 capital

    auto result = allocator.compute_size(intent, snap, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.approved_notional.get() <= 1000.01);  // 10% of $10000
}

TEST_CASE("Allocator: compute_size ограничивает доступным капиталом", "[allocator]") {
    HierarchicalAllocator::Config cfg;
    cfg.max_concentration_pct = 0.80;
    cfg.budget.symbol_budget_pct = 0.80;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 1.0, 50000.0);
    auto snap = make_snapshot(100000.0, 500.0);  // Only $500 available

    auto result = allocator.compute_size(intent, snap, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.approved_notional.get() <= 500.01);
}

// ==================== compute_size_v2 (new API) ====================

TEST_CASE("Allocator v2: базовый ордер с контекстом", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);
    auto snap = make_snapshot();
    auto ctx = make_context();

    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.approved_quantity.get() > 0.0);
    REQUIRE(result.expected_fee > 0.0);
}

TEST_CASE("Allocator v2: constraint_audit заполняется", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);
    auto snap = make_snapshot();
    auto ctx = make_context();

    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.constraint_audit.size() >= 3);  // budget, concentration, strategy, drawdown, liquidity

    // Verify audit trail has expected constraint names
    bool has_budget = false, has_concentration = false, has_strategy = false;
    for (const auto& cd : result.constraint_audit) {
        if (cd.constraint_name == "budget_limit") has_budget = true;
        if (cd.constraint_name == "concentration_limit") has_concentration = true;
        if (cd.constraint_name == "strategy_limit") has_strategy = true;
    }
    REQUIRE(has_budget);
    REQUIRE(has_concentration);
    REQUIRE(has_strategy);
}

TEST_CASE("Allocator v2: drawdown scaling уменьшает размер", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    cfg.drawdown_scale_start_pct = 5.0;
    cfg.drawdown_scale_max_pct = 15.0;
    cfg.drawdown_min_size_fraction = 0.1;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);
    auto snap = make_snapshot();

    auto ctx_no_dd = make_context();
    ctx_no_dd.current_drawdown_pct = 0.0;
    auto result_no_dd = allocator.compute_size_v2(intent, snap, ctx_no_dd, 1.0);

    auto ctx_dd = make_context();
    ctx_dd.current_drawdown_pct = 10.0;  // 10% drawdown = should reduce
    auto result_dd = allocator.compute_size_v2(intent, snap, ctx_dd, 1.0);

    REQUIRE(result_no_dd.approved);
    REQUIRE(result_dd.approved);
    REQUIRE(result_dd.approved_quantity.get() < result_no_dd.approved_quantity.get());
}

TEST_CASE("Allocator v2: liquidity cap ограничивает по ADV", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    cfg.max_concentration_pct = 0.80;
    cfg.budget.symbol_budget_pct = 0.80;
    cfg.max_adv_participation_pct = 0.02;  // 2%
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.1, 50000.0);  // $5000
    auto snap = make_snapshot(100000.0, 100000.0);

    auto ctx = make_context();
    ctx.avg_daily_volume = 10000.0;  // ADV = $10k → 2% = $200 cap
    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);

    REQUIRE(result.approved);
    REQUIRE(result.approved_notional.get() <= 200.01);
}

TEST_CASE("Allocator v2: exchange filters округляют qty", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    cfg.max_concentration_pct = 0.50;
    cfg.budget.symbol_budget_pct = 0.50;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.0155, 50000.0);
    auto snap = make_snapshot();
    auto ctx = make_context();

    ExchangeFilters filters;
    filters.symbol = Symbol("BTCUSDT");
    filters.min_quantity = 0.001;
    filters.quantity_step = 0.001;
    filters.min_notional = 5.0;
    filters.taker_fee_pct = 0.001;
    ctx.exchange_filters = filters;

    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);
    REQUIRE(result.approved);
    // Qty should be rounded to step of 0.001
    double remainder = std::fmod(result.approved_quantity.get(), 0.001);
    REQUIRE_THAT(remainder, WithinAbs(0.0, 1e-9));
}

TEST_CASE("Allocator v2: exchange filters отклоняют при min_notional", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.000001, 50000.0);  // $0.05 notional
    auto snap = make_snapshot(0.5, 0.5);  // Only $0.50 available

    auto ctx = make_context();
    ExchangeFilters filters;
    filters.min_quantity = 0.001;   // min qty = $50
    filters.quantity_step = 0.001;
    filters.min_notional = 10.0;    // min $10
    ctx.exchange_filters = filters;

    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);
    REQUIRE_FALSE(result.approved);
}

TEST_CASE("Allocator v2: fee_adjusted_notional включает комиссию", "[allocator][v2]") {
    HierarchicalAllocator::Config cfg;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);  // $500 notional
    auto snap = make_snapshot();
    auto ctx = make_context();

    auto result = allocator.compute_size_v2(intent, snap, ctx, 1.0);
    REQUIRE(result.approved);
    REQUIRE(result.fee_adjusted_notional > result.approved_notional.get());
    REQUIRE(result.expected_fee > 0.0);
}

// ==================== Utility functions ====================

TEST_CASE("round_quantity_down округляет корректно", "[allocator][utils]") {
    ExchangeFilters f;
    f.quantity_step = 0.001;

    REQUIRE_THAT(round_quantity_down(0.1234, f), WithinAbs(0.123, 1e-9));
    REQUIRE_THAT(round_quantity_down(0.001, f), WithinAbs(0.001, 1e-9));
    REQUIRE_THAT(round_quantity_down(0.0009, f), WithinAbs(0.0, 1e-9));
}

TEST_CASE("round_price округляет корректно", "[allocator][utils]") {
    ExchangeFilters f;
    f.tick_size = 0.01;

    REQUIRE_THAT(round_price(50000.123, f), WithinAbs(50000.12, 1e-6));
    REQUIRE_THAT(round_price(50000.005, f), WithinAbs(50000.01, 1e-6));
}

TEST_CASE("validate_order_filters проверяет границы", "[allocator][utils]") {
    ExchangeFilters f;
    f.min_quantity = 0.001;
    f.max_quantity = 1000.0;
    f.min_notional = 5.0;

    REQUIRE(validate_order_filters(0.01, 500.0, f));
    REQUIRE_FALSE(validate_order_filters(0.0001, 5.0, f));  // qty too small
    REQUIRE_FALSE(validate_order_filters(2000.0, 100000.0, f));  // qty too large
    REQUIRE_FALSE(validate_order_filters(0.01, 0.5, f));  // notional too small
}

// ==================== Volatility targeting ====================

TEST_CASE("Allocator: set_market_context влияет на compute_size", "[allocator][vol]") {
    HierarchicalAllocator::Config cfg;
    cfg.target_annual_vol = 0.15;
    auto allocator = HierarchicalAllocator(cfg, std::make_shared<TestLogger>());

    auto intent = make_intent("BTCUSDT", 0.01, 50000.0);
    auto snap = make_snapshot();

    // Low vol → should increase size
    allocator.set_market_context(0.05, regime::DetailedRegime::StrongUptrend, 0.6, 2.0);
    auto result_low_vol = allocator.compute_size(intent, snap, 1.0);

    // High vol → should decrease size
    allocator.set_market_context(0.80, regime::DetailedRegime::VolatilityExpansion, 0.45, 1.0);
    auto result_high_vol = allocator.compute_size(intent, snap, 1.0);

    REQUIRE(result_low_vol.approved);
    REQUIRE(result_high_vol.approved);
    REQUIRE(result_low_vol.approved_quantity.get() > result_high_vol.approved_quantity.get());
}
