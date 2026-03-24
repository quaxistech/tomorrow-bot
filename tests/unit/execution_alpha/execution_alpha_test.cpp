#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "execution_alpha/execution_alpha_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

using namespace tb;
using namespace tb::execution_alpha;
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
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
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

static features::FeatureSnapshot make_features(double spread_bps = 5.0,
                                                 double aggressive_flow = 0.3,
                                                 double book_instability = 0.2) {
    features::FeatureSnapshot fs;
    fs.symbol = Symbol("BTCUSDT");
    fs.computed_at = Timestamp(1000000);
    fs.market_data_age_ns = Timestamp(100000);
    fs.last_price = Price(50000.0);
    fs.mid_price = Price(50000.0);

    fs.technical.sma_valid = true;
    fs.technical.sma_20 = 49500.0;
    fs.technical.ema_valid = true;
    fs.technical.ema_20 = 49800.0;
    fs.technical.ema_50 = 49600.0;
    fs.technical.volatility_valid = true;
    fs.technical.volatility_5 = 0.02;
    fs.technical.volatility_20 = 0.015;
    fs.technical.momentum_valid = true;
    fs.technical.momentum_5 = 0.01;
    fs.technical.momentum_20 = 0.005;

    fs.microstructure.spread_valid = true;
    fs.microstructure.spread = 10.0;
    fs.microstructure.spread_bps = spread_bps;
    fs.microstructure.trade_flow_valid = true;
    fs.microstructure.aggressive_flow = aggressive_flow;
    fs.microstructure.buy_sell_ratio = 1.0;
    fs.microstructure.instability_valid = true;
    fs.microstructure.book_instability = book_instability;
    fs.microstructure.liquidity_valid = true;
    fs.microstructure.bid_depth_5_notional = 500000.0;
    fs.microstructure.ask_depth_5_notional = 500000.0;
    fs.microstructure.mid_price = 50000.0;

    fs.execution_context.spread_cost_bps = spread_bps;
    fs.execution_context.estimated_slippage_bps = 2.0;
    fs.execution_context.immediate_liquidity = 10000.0;
    fs.execution_context.slippage_valid = true;
    fs.execution_context.is_market_open = true;
    fs.execution_context.is_feed_fresh = true;

    fs.book_quality = order_book::BookQuality::Valid;

    return fs;
}

static strategy::TradeIntent make_intent(double urgency = 0.5,
                                          double conviction = 0.7) {
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("momentum_v1");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.suggested_quantity = Quantity(0.1);
    intent.limit_price = Price(50000.0);
    intent.conviction = conviction;
    intent.urgency = urgency;
    intent.signal_name = "momentum_signal";
    intent.generated_at = Timestamp(999000);
    intent.correlation_id = CorrelationId("test-corr-1");
    return intent;
}

static RuleBasedExecutionAlpha::Config default_config() {
    return RuleBasedExecutionAlpha::Config{};
}

static RuleBasedExecutionAlpha make_engine(RuleBasedExecutionAlpha::Config config = default_config()) {
    return RuleBasedExecutionAlpha(
        config,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());
}

// ========== Тесты ==========

TEST_CASE("ExecutionAlpha: Широкий спред → NoExecution", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    // Спред 60 бп > макс 50 бп
    auto features = make_features(60.0);

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Высокая срочность → Aggressive", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.9, 0.8); // Высокая срочность
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.recommended_style == ExecutionStyle::Aggressive);
    REQUIRE(result.should_execute);
    REQUIRE(result.urgency_score > 0.8);
}

TEST_CASE("ExecutionAlpha: Низкая срочность, узкий спред → Passive", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.2, 0.5); // Низкая срочность
    auto features = make_features(5.0);  // Узкий спред

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.recommended_style == ExecutionStyle::Passive);
    REQUIRE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Токсичный поток → NoExecution", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    // Высокий aggressive_flow + book_instability + умеренный спред → высокий adverse selection
    auto features = make_features(40.0, 0.9, 0.9);

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Вероятность заполнения в допустимом диапазоне", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.quality.fill_probability >= 0.0);
    REQUIRE(result.quality.fill_probability <= 1.0);
}

TEST_CASE("ExecutionAlpha: Лимитная цена корректна для пассивного стиля", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.2, 0.5);
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features);

    REQUIRE(result.recommended_style == ExecutionStyle::Passive);
    REQUIRE(result.recommended_limit_price.has_value());

    // Для покупки лимитная цена должна быть ниже mid
    REQUIRE(result.recommended_limit_price->get() < features.mid_price.get());
}

TEST_CASE("ExecutionAlpha: to_string работает корректно", "[execution_alpha]") {
    REQUIRE(to_string(ExecutionStyle::Passive) == "Passive");
    REQUIRE(to_string(ExecutionStyle::Aggressive) == "Aggressive");
    REQUIRE(to_string(ExecutionStyle::Hybrid) == "Hybrid");
    REQUIRE(to_string(ExecutionStyle::PostOnly) == "PostOnly");
    REQUIRE(to_string(ExecutionStyle::NoExecution) == "NoExecution");
}
