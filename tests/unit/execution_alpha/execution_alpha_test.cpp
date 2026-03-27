#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
#include "execution_alpha/execution_alpha_engine.hpp"
#include "uncertainty/uncertainty_types.hpp"
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

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Высокая срочность → Aggressive", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.9, 0.8); // Высокая срочность
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::Aggressive);
    REQUIRE(result.should_execute);
    REQUIRE(result.urgency_score > 0.8);
}

TEST_CASE("ExecutionAlpha: Низкая срочность, узкий спред → Passive", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.2, 0.5); // Низкая срочность
    auto features = make_features(5.0);  // Узкий спред

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::Passive);
    REQUIRE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Токсичный поток → NoExecution", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    // Высокий aggressive_flow + book_instability + умеренный спред → высокий adverse selection
    auto features = make_features(40.0, 0.9, 0.9);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
}

TEST_CASE("ExecutionAlpha: Вероятность заполнения в допустимом диапазоне", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.quality.fill_probability >= 0.0);
    REQUIRE(result.quality.fill_probability <= 1.0);
}

TEST_CASE("ExecutionAlpha: Лимитная цена корректна для пассивного стиля", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.2, 0.5);
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

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

// ========== Новые тесты: VPIN, имбаланс, PostOnly, валидация фич ==========

TEST_CASE("ExecutionAlpha: VPIN токсичен → NoExecution", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.3, 0.6);
    auto features = make_features(5.0, 0.3, 0.2);

    // VPIN сильно превышает toxic_threshold (0.65)
    features.microstructure.vpin = 0.90;
    features.microstructure.vpin_valid = true;
    features.microstructure.vpin_toxic = true;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
    REQUIRE(result.decision_factors.vpin_used);
    REQUIRE(result.decision_factors.vpin_toxicity > 0.8);
}

TEST_CASE("ExecutionAlpha: VPIN используется → vpin_used=true", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.2, 0.5);
    auto features = make_features(5.0);

    // Низкий VPIN — безопасно, должен быть зафиксирован
    features.microstructure.vpin = 0.30;
    features.microstructure.vpin_valid = true;
    features.microstructure.vpin_toxic = false;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.should_execute);
    REQUIRE(result.decision_factors.vpin_used);
    REQUIRE(result.decision_factors.vpin_toxicity < 0.5);
}

TEST_CASE("ExecutionAlpha: Благоприятный имбаланс стакана → Passive предпочтителен", "[execution_alpha]") {
    // Ситуация: умеренная срочность, но сильный buy imbalance для покупки
    // → должен быть Passive, а не Hybrid
    RuleBasedExecutionAlpha::Config cfg;
    cfg.urgency_passive_threshold = 0.5;
    cfg.imbalance_favorable_threshold = 0.30;
    cfg.max_spread_bps_passive = 15.0;
    auto engine = make_engine(cfg);

    auto intent = make_intent(0.42, 0.6); // Срочность почти на пороге
    intent.side = Side::Buy;
    auto features = make_features(5.0, 0.2, 0.1);

    // book_imbalance_5 > 0 = bid > ask = давление покупателей = favorable для passive BUY
    features.microstructure.book_imbalance_5 = 0.60;
    features.microstructure.book_imbalance_valid = true;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    // С благоприятным имбалансом и умеренной срочностью должен выбрать Passive
    REQUIRE(result.should_execute);
    REQUIRE(result.decision_factors.imbalance_used);
    REQUIRE(result.decision_factors.directional_imbalance > 0.0);
    // Passive или PostOnly (оба корректны при низком adverse)
    REQUIRE((result.recommended_style == ExecutionStyle::Passive
             || result.recommended_style == ExecutionStyle::PostOnly));
}

TEST_CASE("ExecutionAlpha: Неблагоприятный имбаланс → переход к Hybrid", "[execution_alpha]") {
    auto engine = make_engine();
    // Умеренная срочность в зоне Passive, но имбаланс стакана против нас
    auto intent = make_intent(0.35, 0.6);
    intent.side = Side::Buy;
    auto features = make_features(8.0, 0.2, 0.2);

    // Отрицательный book_imbalance_5 для Buy = против нас (ask > bid = sell давление)
    features.microstructure.book_imbalance_5 = -0.60; // Сильно против нас
    features.microstructure.book_imbalance_valid = true;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.should_execute);
    REQUIRE(result.decision_factors.directional_imbalance < -0.30);
    // Неблагоприятный имбаланс → не Passive, а Hybrid
    REQUIRE(result.recommended_style != ExecutionStyle::Passive);
}

TEST_CASE("ExecutionAlpha: CUSUM сигнал увеличивает срочность", "[execution_alpha]") {
    auto engine = make_engine();

    // Без CUSUM
    auto intent_no_cusum = make_intent(0.3, 0.6);
    auto features_no_cusum = make_features(5.0);
    features_no_cusum.technical.cusum_valid = false;
    features_no_cusum.technical.cusum_regime_change = false;
    auto result_no_cusum = engine.evaluate(intent_no_cusum, features_no_cusum, uncertainty::UncertaintySnapshot{});

    // С CUSUM
    auto intent_cusum = make_intent(0.3, 0.6);
    auto features_cusum = make_features(5.0);
    features_cusum.technical.cusum_valid = true;
    features_cusum.technical.cusum_regime_change = true;
    auto result_cusum = engine.evaluate(intent_cusum, features_cusum, uncertainty::UncertaintySnapshot{});

    // CUSUM должен повысить срочность
    REQUIRE(result_cusum.urgency_score > result_no_cusum.urgency_score);
    REQUIRE(result_cusum.decision_factors.urgency_cusum_adj > 0.0);
    REQUIRE(result_no_cusum.decision_factors.urgency_cusum_adj == 0.0);
}

TEST_CASE("ExecutionAlpha: PostOnly при идеальных условиях", "[execution_alpha]") {
    RuleBasedExecutionAlpha::Config cfg;
    cfg.postonly_spread_threshold_bps = 4.5;
    cfg.postonly_urgency_max = 0.35;
    cfg.postonly_adverse_max = 0.35;
    auto engine = make_engine(cfg);

    // Очень узкий спред + очень низкая срочность + чистый поток
    auto intent = make_intent(0.05, 0.5); // urgency=0.05 << 0.35
    auto features = make_features(2.0, 0.05, 0.05); // spread_bps=2.0 << 4.5

    features.microstructure.vpin = 0.20;
    features.microstructure.vpin_valid = true;
    features.microstructure.vpin_toxic = false;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.should_execute);
    REQUIRE(result.recommended_style == ExecutionStyle::PostOnly);
}

TEST_CASE("ExecutionAlpha: Невалидные данные (mid_price=0) → NoExecution", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5, 0.7);
    auto features = make_features(5.0);

    // Испорченные данные
    features.mid_price = Price(0.0);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::NoExecution);
    REQUIRE_FALSE(result.should_execute);
    REQUIRE_FALSE(result.decision_factors.features_complete);
}

TEST_CASE("ExecutionAlpha: DecisionFactors заполняются корректно", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.3, 0.6);
    auto features = make_features(5.0, 0.25, 0.15);

    features.microstructure.vpin = 0.40;
    features.microstructure.vpin_valid = true;
    features.microstructure.book_imbalance_5 = 0.20;
    features.microstructure.book_imbalance_valid = true;

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.should_execute);
    const auto& df = result.decision_factors;

    // Все компоненты должны быть заполнены
    REQUIRE(df.features_complete);
    REQUIRE(df.vpin_used);
    REQUIRE(df.imbalance_used);
    REQUIRE(df.urgency_base == Catch::Approx(0.3));
    REQUIRE(df.adverse_selection_score >= 0.0);
    REQUIRE(df.adverse_selection_score <= 1.0);
    REQUIRE(df.directional_imbalance == Catch::Approx(0.20));

    // Urgency должен быть ≥ base (добавляются положительные поправки за вол/моментум)
    REQUIRE(result.urgency_score >= df.urgency_base);
}

TEST_CASE("ExecutionAlpha: Data-driven fill_prob варьируется с условиями стакана", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent_buy = make_intent(0.2, 0.5);
    intent_buy.side = Side::Buy;

    // Узкий спред, хорошая глубина
    auto features_tight = make_features(2.0);
    features_tight.microstructure.bid_depth_5_notional = 1000000.0;

    // Широкий спред, мелкая глубина
    auto features_wide = make_features(13.0);
    features_wide.microstructure.bid_depth_5_notional = 5000.0;

    auto result_tight = engine.evaluate(intent_buy, features_tight, uncertainty::UncertaintySnapshot{});
    auto result_wide  = engine.evaluate(intent_buy, features_wide, uncertainty::UncertaintySnapshot{});

    // Оба должны исполниться, но tight должен иметь выше fill_prob
    if (result_tight.should_execute && result_wide.should_execute) {
        REQUIRE(result_tight.quality.fill_probability
                > result_wide.quality.fill_probability);
    }
}

TEST_CASE("ExecutionAlpha: Aggressive → нет лимитной цены", "[execution_alpha]") {
    auto engine = make_engine();
    auto intent = make_intent(0.95, 0.9); // Очень высокая срочность → Aggressive
    auto features = make_features(5.0);

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.recommended_style == ExecutionStyle::Aggressive);
    // Для рыночного ордера лимитная цена не нужна
    REQUIRE_FALSE(result.recommended_limit_price.has_value());
}

TEST_CASE("ExecutionAlpha: weighted_mid используется при наличии", "[execution_alpha]") {
    RuleBasedExecutionAlpha::Config cfg;
    cfg.use_weighted_mid_price = true;
    auto engine = make_engine(cfg);

    auto intent = make_intent(0.2, 0.5);
    auto features = make_features(5.0);

    // Взвешенная средняя немного отличается от mid
    features.microstructure.weighted_mid_price = 50010.0; // Bid-heavy → weighted_mid выше

    auto result = engine.evaluate(intent, features, uncertainty::UncertaintySnapshot{});

    REQUIRE(result.should_execute);
    REQUIRE(result.decision_factors.weighted_mid_used);
}

