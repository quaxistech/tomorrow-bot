#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
#include "risk/risk_engine.hpp"
#include "portfolio/portfolio_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "order_book/order_book_types.hpp"
#include "regime/regime_types.hpp"

using namespace tb;
using namespace tb::risk;
using namespace Catch::Matchers;

// ========== Тестовые заглушки ==========

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

class TestClock : public clock::IClock {
public:
    int64_t current_time{1'000'000'000LL};
    [[nodiscard]] Timestamp now() const override { return Timestamp(current_time); }
};

class TestMetrics : public metrics::IMetricsRegistry {
    struct NullCounter : metrics::ICounter {
        std::string name_{"null"};
        void increment(double /*v*/) override {}
        void increment(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullGauge : metrics::IGauge {
        std::string name_{"null"};
        void set(double /*v*/) override {}
        void set(double /*v*/, const metrics::MetricTags&) override {}
        void increment(double /*v*/) override {}
        void decrement(double /*v*/) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullHistogram : metrics::IHistogram {
        std::string name_{"null"};
        void observe(double /*v*/) override {}
        void observe(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override {
        return std::make_shared<NullCounter>();
    }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override {
        return std::make_shared<NullGauge>();
    }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override {
        return std::make_shared<NullHistogram>();
    }
    [[nodiscard]] std::string export_prometheus() const override { return ""; }
};

// ========== Вспомогательные функции ==========

static ExtendedRiskConfig default_risk_config() {
    ExtendedRiskConfig cfg;
    cfg.max_position_notional = 10000.0;
    cfg.max_daily_loss_pct = 2.0;
    cfg.max_drawdown_pct = 5.0;
    cfg.max_concurrent_positions = 5;
    cfg.max_gross_exposure_pct = 50.0;
    cfg.max_leverage = 3.0;
    cfg.max_slippage_bps = 30.0;
    cfg.max_orders_per_minute = 10;
    cfg.max_consecutive_losses = 5;
    cfg.max_spread_bps = 50.0;
    cfg.min_liquidity_depth = 100.0;
    cfg.max_feed_age_ns = 5'000'000'000LL;
    cfg.kill_switch_enabled = true;
    return cfg;
}

static strategy::TradeIntent make_intent() {
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("test");
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.suggested_quantity = Quantity(0.1);
    intent.conviction = 0.7;
    intent.correlation_id = CorrelationId("test-1");
    return intent;
}

static portfolio_allocator::SizingResult make_sizing(double notional = 5000.0) {
    portfolio_allocator::SizingResult sizing;
    sizing.approved_quantity = Quantity(0.1);
    sizing.approved_notional = NotionalValue(notional);
    sizing.approved = true;
    return sizing;
}

static portfolio::PortfolioSnapshot make_clean_portfolio() {
    portfolio::PortfolioSnapshot snap;
    snap.total_capital = 100000.0;
    snap.available_capital = 90000.0;
    snap.exposure.gross_exposure = 10000.0;
    snap.exposure.open_positions_count = 1;
    snap.pnl.total_pnl = 0.0;
    snap.pnl.current_drawdown_pct = 0.0;
    snap.pnl.consecutive_losses = 0;
    return snap;
}

static features::FeatureSnapshot make_clean_features() {
    features::FeatureSnapshot fs;
    fs.symbol = Symbol("BTCUSDT");
    fs.computed_at = Timestamp(1000000);
    fs.market_data_age_ns = Timestamp(100000);
    fs.last_price = Price(50000.0);
    fs.mid_price = Price(50000.0);

    fs.technical.sma_valid = true;
    fs.microstructure.spread_valid = true;
    fs.microstructure.spread_bps = 5.0;
    fs.microstructure.liquidity_valid = true;
    fs.microstructure.bid_depth_5_notional = 500000.0;
    fs.microstructure.ask_depth_5_notional = 500000.0;

    fs.execution_context.is_feed_fresh = true;
    fs.execution_context.is_market_open = true;
    fs.execution_context.slippage_valid = true;
    fs.execution_context.estimated_slippage_bps = 2.0;

    fs.book_quality = order_book::BookQuality::Valid;
    return fs;
}

static execution_alpha::ExecutionAlphaResult make_clean_exec_alpha() {
    execution_alpha::ExecutionAlphaResult result;
    result.recommended_style = execution_alpha::ExecutionStyle::Passive;
    result.quality.estimated_slippage_bps = 2.0;
    result.quality.total_cost_bps = 5.0;
    result.should_execute = true;
    return result;
}

static std::shared_ptr<ProductionRiskEngine> make_risk_engine(
    ExtendedRiskConfig config = default_risk_config())
{
    return std::make_shared<ProductionRiskEngine>(
        config,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());
}

// ========== Тесты 14 правил ==========

TEST_CASE("Risk: Чистое состояние → Approved", "[risk]") {
    auto engine = make_risk_engine();
    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Approved);
    REQUIRE(decision.reasons.empty());
}

TEST_CASE("Risk: Правило 1 — Kill switch → Denied", "[risk]") {
    auto engine = make_risk_engine();
    engine->activate_kill_switch("Тестовая причина");

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    REQUIRE(decision.kill_switch_active);
    REQUIRE_FALSE(decision.reasons.empty());
    REQUIRE(decision.reasons[0].code == "KILL_SWITCH");
}

TEST_CASE("Risk: Правило 2 — Макс дневной убыток → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto portfolio = make_clean_portfolio();
    // Убыток 3% при лимите 2%
    portfolio.pnl.total_pnl = -3000.0; // -3% от 100000
    portfolio.pnl.realized_pnl_today = -3000.0; // Реализованный убыток

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_DAILY_LOSS") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 3 — Макс просадка → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto portfolio = make_clean_portfolio();
    portfolio.pnl.current_drawdown_pct = 6.0; // >5% лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_DRAWDOWN") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 4 — Макс позиций → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto portfolio = make_clean_portfolio();
    portfolio.exposure.open_positions_count = 5; // =max

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_POSITIONS") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 5 — Макс экспозиция → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto portfolio = make_clean_portfolio();
    portfolio.exposure.gross_exposure = 60000.0; // 60% > 50% лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_EXPOSURE") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 6 — Макс номинал → ReduceSize", "[risk]") {
    auto engine = make_risk_engine();
    // Номинал 15000 > лимит 10000
    auto sizing = make_sizing(15000.0);

    auto decision = engine->evaluate(
        make_intent(), sizing, make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    // Должен быть ReduceSize (не Denied, т.к. можно уменьшить)
    bool has_max_notional = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_NOTIONAL") has_max_notional = true;
    }
    REQUIRE(has_max_notional);
    // Одобренный объём должен быть уменьшен
    REQUIRE(decision.approved_quantity.get() < sizing.approved_quantity.get());
}

TEST_CASE("Risk: Правило 7 — Макс плечо → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto portfolio = make_clean_portfolio();
    portfolio.total_capital = 10000.0;
    portfolio.exposure.gross_exposure = 35000.0; // 3.5x > 3.0x

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_LEVERAGE") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 8 — Макс проскальзывание → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto exec_alpha = make_clean_exec_alpha();
    exec_alpha.quality.estimated_slippage_bps = 50.0; // >30 бп лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), exec_alpha);

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "MAX_SLIPPAGE") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 9 — Частота ордеров → Throttled", "[risk]") {
    auto engine = make_risk_engine();

    // Зафиксировать 10 ордеров (= лимит)
    for (int i = 0; i < 10; ++i) {
        engine->record_order_sent();
    }

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Throttled);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "ORDER_RATE") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 10 — Подряд убытки → Denied", "[risk]") {
    auto engine = make_risk_engine();

    // consecutive_losses теперь отслеживаются в PortfolioEngine
    auto portfolio = make_clean_portfolio();
    portfolio.pnl.consecutive_losses = 5; // = лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "CONSECUTIVE_LOSSES") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 11 — Устаревшие данные → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto features = make_clean_features();
    features.execution_context.is_feed_fresh = false; // Данные устарели

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        features, make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "STALE_FEED") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 12 — Невалидный стакан → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto features = make_clean_features();
    features.book_quality = order_book::BookQuality::Stale;

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        features, make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "INVALID_BOOK") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 13 — Широкий спред → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto features = make_clean_features();
    features.microstructure.spread_bps = 60.0; // >50 бп лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        features, make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "WIDE_SPREAD") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 14 — Низкая ликвидность → Denied", "[risk]") {
    auto engine = make_risk_engine();
    auto features = make_clean_features();
    features.microstructure.bid_depth_5_notional = 20.0;
    features.microstructure.ask_depth_5_notional = 20.0; // 40 < 100

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        features, make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "LOW_LIQUIDITY") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Множественные нарушения → все причины перечислены", "[risk]") {
    auto engine = make_risk_engine();
    engine->activate_kill_switch("Тест");

    auto portfolio = make_clean_portfolio();
    portfolio.pnl.total_pnl = -3000.0; // Дневной убыток
    portfolio.pnl.realized_pnl_today = -3000.0; // Реализованный дневной убыток

    auto features = make_clean_features();
    features.execution_context.is_feed_fresh = false; // Устаревшие данные

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        features, make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    REQUIRE(decision.reasons.size() >= 3);
}

TEST_CASE("Risk: to_string работает корректно", "[risk]") {
    REQUIRE(to_string(RiskVerdict::Approved) == "Approved");
    REQUIRE(to_string(RiskVerdict::Denied) == "Denied");
    REQUIRE(to_string(RiskVerdict::ReduceSize) == "ReduceSize");
    REQUIRE(to_string(RiskVerdict::Throttled) == "Throttled");

    REQUIRE(to_string(RiskPhase::PreTrade) == "PreTrade");
    REQUIRE(to_string(RiskPhase::IntraTrade) == "IntraTrade");
    REQUIRE(to_string(RiskPhase::PostTrade) == "PostTrade");
}

TEST_CASE("Risk: Деактивация kill switch", "[risk]") {
    auto engine = make_risk_engine();
    engine->activate_kill_switch("Тест");
    REQUIRE(engine->is_kill_switch_active());

    engine->deactivate_kill_switch();
    REQUIRE_FALSE(engine->is_kill_switch_active());

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());
    REQUIRE(decision.verdict == RiskVerdict::Approved);
}

// ========== Тесты новых правил 16-23 ==========

static std::shared_ptr<ProductionRiskEngine> make_risk_engine_with_clock(
    ExtendedRiskConfig config,
    std::shared_ptr<TestClock> clock)
{
    return std::make_shared<ProductionRiskEngine>(
        config,
        std::make_shared<TestLogger>(),
        clock,
        std::make_shared<TestMetrics>());
}

TEST_CASE("Risk: Правило 16 — Бюджет стратегии → Denied", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_strategy_daily_loss_pct = 1.5; // 1.5% от капитала
    auto engine = make_risk_engine(cfg);

    // Записываем убыточную сделку от стратегии "test"
    engine->record_trade_close(StrategyId("test"), Symbol("BTCUSDT"), -2000.0);

    auto portfolio = make_clean_portfolio(); // total_capital = 100000
    // 2000 / 100000 = 2% > 1.5% лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "STRATEGY_BUDGET") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 17 — Концентрация символа → Denied", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_symbol_concentration_pct = 35.0;
    auto engine = make_risk_engine(cfg);

    auto portfolio = make_clean_portfolio();
    // Добавляем позицию BTCUSDT с номиналом 40% капитала
    portfolio::Position pos;
    pos.symbol = Symbol("BTCUSDT");
    pos.side = Side::Buy;
    pos.notional = NotionalValue(40000.0); // 40% от 100000
    pos.unrealized_pnl = 100.0;
    portfolio.positions.push_back(pos);

    auto intent = make_intent(); // symbol = BTCUSDT

    auto decision = engine->evaluate(
        intent, make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "SYMBOL_CONCENTRATION") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 18 — Однонаправленные позиции → Denied", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_same_direction_positions = 3;
    auto engine = make_risk_engine(cfg);

    auto portfolio = make_clean_portfolio();
    // Добавляем 3 BUY-позиции (= лимит)
    for (int i = 0; i < 3; ++i) {
        portfolio::Position pos;
        pos.symbol = Symbol("SYM" + std::to_string(i));
        pos.side = Side::Buy;
        pos.notional = NotionalValue(1000.0);
        pos.unrealized_pnl = 10.0;
        portfolio.positions.push_back(pos);
    }

    auto intent = make_intent(); // side = Buy

    auto decision = engine->evaluate(
        intent, make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "SAME_DIRECTION") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 19 — UTC cutoff → Denied", "[risk]") {
    auto cfg = default_risk_config();
    cfg.utc_cutoff_hour = 20; // Прекращение торговли после 20:00 UTC
    auto clock = std::make_shared<TestClock>();
    // Устанавливаем время на 21:00 UTC (21 * 3600 * 1e9 нс)
    clock->current_time = 21LL * 3600LL * 1'000'000'000LL;
    auto engine = make_risk_engine_with_clock(cfg, clock);

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "UTC_CUTOFF") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 20 — Оборачиваемость → Throttled", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_trades_per_hour = 8;
    auto engine = make_risk_engine(cfg);

    // Записываем 8 сделок (= лимит)
    for (int i = 0; i < 8; ++i) {
        engine->record_trade_close(StrategyId("test"), Symbol("BTCUSDT"), 10.0);
    }

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Throttled);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "TURNOVER_RATE") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 21 — Реализованный дневной убыток → Denied", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_realized_daily_loss_pct = 1.5;
    auto engine = make_risk_engine(cfg);

    auto portfolio = make_clean_portfolio();
    portfolio.pnl.realized_pnl_today = -2000.0; // 2% > 1.5% лимит

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), portfolio,
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Denied);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "REALIZED_DAILY_LOSS") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Правило 22 — Интервал сделок → Throttled", "[risk]") {
    auto cfg = default_risk_config();
    cfg.min_trade_interval_ns = 30'000'000'000LL; // 30с
    auto clock = std::make_shared<TestClock>();
    clock->current_time = 100'000'000'000LL; // 100с
    auto engine = make_risk_engine_with_clock(cfg, clock);

    // Записываем сделку — timestamp будет 100с
    engine->record_trade_close(StrategyId("test"), Symbol("BTCUSDT"), 10.0);

    // Время продвигается на 10с (< 30с интервал)
    clock->current_time = 110'000'000'000LL;

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.verdict == RiskVerdict::Throttled);
    bool found = false;
    for (const auto& r : decision.reasons) {
        if (r.code == "TRADE_INTERVAL") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Intra-trade — Макс удержание → should_close", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_position_hold_ns = 3'600'000'000'000LL; // 1 час
    auto clock = std::make_shared<TestClock>();
    clock->current_time = 7'200'000'000'000LL; // 2 часа
    auto engine = make_risk_engine_with_clock(cfg, clock);

    portfolio::Position pos;
    pos.symbol = Symbol("BTCUSDT");
    pos.side = Side::Buy;
    pos.opened_at = Timestamp(0LL); // Открыта в начале — 2 часа назад
    pos.unrealized_pnl = 100.0;

    auto assessment = engine->evaluate_position(
        pos, make_clean_portfolio(), make_clean_features());

    REQUIRE(assessment.should_close);
    bool found = false;
    for (const auto& r : assessment.reasons) {
        if (r.code == "MAX_HOLD_TIME") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: Intra-trade — Макс adverse excursion → should_close", "[risk]") {
    auto cfg = default_risk_config();
    cfg.max_adverse_excursion_pct = 3.0;
    auto engine = make_risk_engine(cfg);

    portfolio::Position pos;
    pos.symbol = Symbol("BTCUSDT");
    pos.side = Side::Buy;
    pos.opened_at = Timestamp(0LL);
    pos.unrealized_pnl = -4000.0; // 4% от 100000 > 3% лимит

    auto portfolio = make_clean_portfolio();

    auto assessment = engine->evaluate_position(
        pos, portfolio, make_clean_features());

    REQUIRE(assessment.should_close);
    bool found = false;
    for (const auto& r : assessment.reasons) {
        if (r.code == "MAX_ADVERSE_EXCURSION") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Risk: record_trade_close обновляет стратегический бюджет", "[risk]") {
    auto engine = make_risk_engine();

    engine->record_trade_close(StrategyId("alpha"), Symbol("BTCUSDT"), -500.0);
    engine->record_trade_close(StrategyId("alpha"), Symbol("ETHUSDT"), -300.0);
    engine->record_trade_close(StrategyId("alpha"), Symbol("BTCUSDT"), 200.0);

    auto snapshot = engine->get_risk_snapshot();

    REQUIRE(snapshot.strategy_budgets.size() == 1);
    REQUIRE(snapshot.strategy_budgets[0].strategy_id.get() == "alpha");
    REQUIRE(snapshot.strategy_budgets[0].daily_loss == Catch::Approx(800.0));
    REQUIRE(snapshot.strategy_budgets[0].trades_today == 3);
    REQUIRE(snapshot.strategy_budgets[0].consecutive_losses == 0); // Последняя сделка — прибыль
}

TEST_CASE("Risk: set_current_regime меняет scaling factor", "[risk]") {
    auto cfg = default_risk_config();
    cfg.regime_aware_limits_enabled = true;
    cfg.stress_regime_scale = 0.5;
    cfg.trending_regime_scale = 1.2;
    auto engine = make_risk_engine(cfg);

    engine->set_current_regime(regime::DetailedRegime::LiquidityStress);

    auto decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.regime_scaling_factor == Catch::Approx(0.5));

    engine->set_current_regime(regime::DetailedRegime::StrongUptrend);

    decision = engine->evaluate(
        make_intent(), make_sizing(), make_clean_portfolio(),
        make_clean_features(), make_clean_exec_alpha());

    REQUIRE(decision.regime_scaling_factor == Catch::Approx(1.2));
}

TEST_CASE("Risk: reset_daily сбрасывает дневные лимиты", "[risk]") {
    auto engine = make_risk_engine();

    // Записываем убытки
    engine->record_trade_close(StrategyId("test"), Symbol("BTCUSDT"), -1000.0);
    engine->record_trade_close(StrategyId("test"), Symbol("ETHUSDT"), -500.0);

    auto snapshot = engine->get_risk_snapshot();
    REQUIRE(snapshot.strategy_budgets.size() == 1);
    REQUIRE(snapshot.strategy_budgets[0].daily_loss == Catch::Approx(1500.0));

    engine->reset_daily();

    snapshot = engine->get_risk_snapshot();
    REQUIRE(snapshot.strategy_budgets.size() == 1);
    REQUIRE(snapshot.strategy_budgets[0].daily_loss == Catch::Approx(0.0));
    REQUIRE(snapshot.strategy_budgets[0].trades_today == 0);
}

TEST_CASE("Risk: get_risk_snapshot возвращает корректный снимок", "[risk]") {
    auto cfg = default_risk_config();
    cfg.regime_aware_limits_enabled = true;
    cfg.stress_regime_scale = 0.5;
    auto engine = make_risk_engine(cfg);

    engine->set_current_regime(regime::DetailedRegime::LiquidityStress);
    engine->record_trade_close(StrategyId("alpha"), Symbol("BTCUSDT"), -100.0);
    engine->record_trade_close(StrategyId("beta"), Symbol("ETHUSDT"), 200.0);

    auto snapshot = engine->get_risk_snapshot();

    REQUIRE_FALSE(snapshot.kill_switch_active);
    REQUIRE(snapshot.regime_scaling_factor == Catch::Approx(0.5));
    REQUIRE(snapshot.strategy_budgets.size() == 2);
    REQUIRE(snapshot.computed_at.get() > 0);
}
