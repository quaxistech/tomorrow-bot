#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "regime/regime_engine.hpp"
#include "metrics/metrics_registry.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::regime;
using namespace tb::features;

namespace {

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
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override { return nullptr; }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override { return nullptr; }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override { return nullptr; }
    std::string export_prometheus() const override { return ""; }
};

FeatureSnapshot make_snapshot() {
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    return snap;
}

} // anonymous namespace

TEST_CASE("Regime: StrongUptrend — EMA20 > EMA50, ADX > 30, RSI 50-70", "[regime]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.ema_20 = 51000.0;
    snap.technical.ema_50 = 49000.0;
    snap.technical.ema_valid = true;
    snap.technical.adx = 35.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 60.0;
    snap.technical.rsi_valid = true;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::StrongUptrend);
    REQUIRE(result.label == RegimeLabel::Trending);
    REQUIRE(result.confidence > 0.5);
}

TEST_CASE("Regime: MeanReversion — экстремальный RSI, низкий ADX", "[regime]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.rsi_14 = 75.0;
    snap.technical.rsi_valid = true;
    snap.technical.adx = 20.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_valid = true;
    snap.technical.ema_20 = 50000.0;
    snap.technical.ema_50 = 50000.0;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::MeanReversion);
    REQUIRE(result.label == RegimeLabel::Ranging);
}

TEST_CASE("Regime: LowVolCompression — низкий bandwidth, низкий ADX", "[regime]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.bb_bandwidth = 0.01;
    snap.technical.bb_valid = true;
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_valid = true;
    snap.technical.rsi_valid = true;
    snap.technical.rsi_14 = 50.0;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::LowVolCompression);
    REQUIRE(result.label == RegimeLabel::Ranging);
}

TEST_CASE("Regime: Chop — низкий ADX, нет направления", "[regime]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_20 = 50000.0;
    snap.technical.ema_50 = 50000.0;
    snap.technical.ema_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::Chop);
    REQUIRE(result.label == RegimeLabel::Ranging);
}

TEST_CASE("Regime: Стратегические рекомендации для тренда", "[regime]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto metrics = std::make_shared<TestMetrics>();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.ema_20 = 51000.0;
    snap.technical.ema_50 = 49000.0;
    snap.technical.ema_valid = true;
    snap.technical.adx = 35.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 60.0;
    snap.technical.rsi_valid = true;

    auto result = engine.classify(snap);

    // Проверяем что momentum рекомендован
    bool momentum_enabled = false;
    bool mean_reversion_disabled = false;
    for (const auto& hint : result.strategy_hints) {
        if (hint.strategy_id.get() == "momentum" && hint.should_enable) {
            momentum_enabled = true;
            REQUIRE(hint.weight_multiplier > 1.0);
        }
        if (hint.strategy_id.get() == "mean_reversion" && !hint.should_enable) {
            mean_reversion_disabled = true;
        }
    }
    REQUIRE(momentum_enabled);
    REQUIRE(mean_reversion_disabled);
}

TEST_CASE("Regime: to_simple_label корректность", "[regime]") {
    REQUIRE(to_simple_label(DetailedRegime::StrongUptrend) == RegimeLabel::Trending);
    REQUIRE(to_simple_label(DetailedRegime::WeakDowntrend) == RegimeLabel::Trending);
    REQUIRE(to_simple_label(DetailedRegime::MeanReversion) == RegimeLabel::Ranging);
    REQUIRE(to_simple_label(DetailedRegime::Chop) == RegimeLabel::Ranging);
    REQUIRE(to_simple_label(DetailedRegime::VolatilityExpansion) == RegimeLabel::Volatile);
    REQUIRE(to_simple_label(DetailedRegime::Undefined) == RegimeLabel::Unclear);
}
