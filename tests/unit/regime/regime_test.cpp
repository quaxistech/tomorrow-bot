#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "regime/regime_engine.hpp"
#include "regime/regime_config.hpp"
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

auto make_deps() {
    struct Deps {
        std::shared_ptr<TestLogger> logger;
        std::shared_ptr<TestClock> clock;
        std::shared_ptr<TestMetrics> metrics;
    };
    return Deps{
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>()
    };
}

FeatureSnapshot make_snapshot() {
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    return snap;
}

FeatureSnapshot make_strong_uptrend() {
    auto snap = make_snapshot();
    snap.technical.ema_20 = 51000.0;
    snap.technical.ema_50 = 49000.0;
    snap.technical.ema_valid = true;
    snap.technical.adx = 35.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 60.0;
    snap.technical.rsi_valid = true;
    return snap;
}

FeatureSnapshot make_chop() {
    auto snap = make_snapshot();
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_20 = 50000.0;
    snap.technical.ema_50 = 50000.0;
    snap.technical.ema_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    return snap;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: Basic regime classification (original tests preserved)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: StrongUptrend — EMA20 > EMA50, ADX > 30, RSI 50-70", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_strong_uptrend());

    REQUIRE(result.detailed == DetailedRegime::StrongUptrend);
    REQUIRE(result.label == RegimeLabel::Trending);
    REQUIRE(result.confidence > 0.5);
}

TEST_CASE("Regime: MeanReversion — экстремальный RSI, низкий ADX", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
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
    auto [logger, clk, metrics] = make_deps();
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
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_chop());

    REQUIRE(result.detailed == DetailedRegime::Chop);
    REQUIRE(result.label == RegimeLabel::Ranging);
}

TEST_CASE("Regime: Стратегические рекомендации для тренда", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_strong_uptrend());

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

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: Config-driven classification
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: custom config changes ADX threshold", "[regime][config]") {
    auto [logger, clk, metrics] = make_deps();

    RegimeConfig cfg;
    cfg.trend.adx_strong = 40.0;  // Raise threshold: ADX=35 should now be weak
    cfg.trend.adx_weak_max = 40.0;  // Extend weak range to cover ADX=35

    RuleBasedRegimeEngine engine(logger, clk, metrics, cfg);

    auto snap = make_strong_uptrend();  // ADX=35
    auto result = engine.classify(snap);

    // ADX=35 < 40 (new strong threshold), but in [18, 40] weak range → WeakUptrend
    REQUIRE(result.detailed == DetailedRegime::WeakUptrend);
}

TEST_CASE("Regime: custom config changes mean-reversion RSI threshold", "[regime][config]") {
    auto [logger, clk, metrics] = make_deps();

    RegimeConfig cfg;
    cfg.mean_reversion.rsi_overbought = 80.0;  // Raise: RSI=75 should no longer trigger MR

    RuleBasedRegimeEngine engine(logger, clk, metrics, cfg);

    auto snap = make_snapshot();
    snap.technical.rsi_14 = 75.0;  // Below new threshold (80)
    snap.technical.rsi_valid = true;
    snap.technical.adx = 20.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_valid = true;
    snap.technical.ema_20 = 50000.0;
    snap.technical.ema_50 = 50000.0;

    auto result = engine.classify(snap);

    // RSI=75 < 80, so not MeanReversion anymore
    REQUIRE(result.detailed != DetailedRegime::MeanReversion);
}

TEST_CASE("Regime: default config produces same results as no config", "[regime][config]") {
    auto [logger, clk, metrics] = make_deps();

    RuleBasedRegimeEngine engine_default(logger, clk, metrics);
    RuleBasedRegimeEngine engine_explicit(logger, clk, metrics, make_default_regime_config());

    auto snap = make_strong_uptrend();

    auto r1 = engine_default.classify(snap);
    auto r2 = engine_explicit.classify(snap);

    REQUIRE(r1.detailed == r2.detailed);
    REQUIRE(r1.label == r2.label);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: Explainability — ClassificationExplanation
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: explanation contains triggered conditions", "[regime][explain]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_strong_uptrend());

    REQUIRE(!result.explanation.triggered_conditions.empty());
    REQUIRE(!result.explanation.checked_conditions.empty());
    REQUIRE(result.explanation.triggered_conditions.size() <=
            result.explanation.checked_conditions.size());
}

TEST_CASE("Regime: explanation summary is non-empty", "[regime][explain]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_strong_uptrend());

    REQUIRE(!result.explanation.summary.empty());
}

TEST_CASE("Regime: explanation tracks data quality", "[regime][explain]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    // Snapshot with valid indicators
    auto snap = make_strong_uptrend();
    auto r1 = engine.classify(snap);
    REQUIRE(r1.explanation.valid_indicator_count > 0);

    // Snapshot with no valid indicators
    auto snap2 = make_snapshot();
    snap2.technical.ema_valid = false;
    snap2.technical.adx_valid = false;
    snap2.technical.rsi_valid = false;
    snap2.technical.bb_valid = false;
    auto r2 = engine.classify(snap2);
    REQUIRE(r2.explanation.valid_indicator_count == 0);
}

TEST_CASE("Regime: explanation immediate matches persistent on first call", "[regime][explain]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto result = engine.classify(make_strong_uptrend());

    // First classification: hysteresis should not override
    REQUIRE(result.explanation.immediate_regime == result.explanation.persistent_regime);
    REQUIRE(!result.explanation.hysteresis_overrode);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 4: Hysteresis and transition logic
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: hysteresis prevents immediate flip", "[regime][hysteresis]") {
    auto [logger, clk, metrics] = make_deps();

    RegimeConfig cfg;
    cfg.transition.confirmation_ticks = 3;
    cfg.transition.dwell_time_ticks = 2;

    RuleBasedRegimeEngine engine(logger, clk, metrics, cfg);

    // Establish a regime first (multiple ticks to pass dwell)
    auto uptrend = make_strong_uptrend();
    for (int i = 0; i < 5; ++i) {
        engine.classify(uptrend);
    }

    // Now try to flip to Chop — should be resisted by hysteresis
    auto chop = make_chop();
    auto result = engine.classify(chop);

    // The immediate classification should be Chop, but persistent should still be StrongUptrend
    REQUIRE(result.explanation.immediate_regime == DetailedRegime::Chop);
    // Hysteresis keeps the old regime until confirmation_ticks is met
    REQUIRE(result.detailed == DetailedRegime::StrongUptrend);
    REQUIRE(result.explanation.hysteresis_overrode == true);
}

TEST_CASE("Regime: hysteresis allows flip after confirmation ticks", "[regime][hysteresis]") {
    auto [logger, clk, metrics] = make_deps();

    RegimeConfig cfg;
    cfg.transition.confirmation_ticks = 2;
    cfg.transition.dwell_time_ticks = 1;
    cfg.transition.min_confidence_to_switch = 0.3;

    RuleBasedRegimeEngine engine(logger, clk, metrics, cfg);

    // Establish uptrend (exceeds dwell_time_ticks=1)
    auto uptrend = make_strong_uptrend();
    engine.classify(uptrend);
    engine.classify(uptrend);

    // Send chop signals for confirmation_ticks rounds
    auto chop = make_chop();
    engine.classify(chop);  // tick 1 — candidate
    engine.classify(chop);  // tick 2 — confirmed

    auto final_result = engine.classify(chop);  // tick 3 — should be chop now
    REQUIRE(final_result.detailed == DetailedRegime::Chop);
}

TEST_CASE("Regime: dwell time prevents premature switch", "[regime][hysteresis]") {
    auto [logger, clk, metrics] = make_deps();

    RegimeConfig cfg;
    cfg.transition.confirmation_ticks = 1;
    cfg.transition.dwell_time_ticks = 10;  // High dwell

    RuleBasedRegimeEngine engine(logger, clk, metrics, cfg);

    // Single uptrend classification
    engine.classify(make_strong_uptrend());

    // Try to switch to chop — dwell time not met
    auto result = engine.classify(make_chop());

    // Should still be uptrend because dwell_time_ticks=10 not met
    REQUIRE(result.detailed == DetailedRegime::StrongUptrend);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 5: Stress regimes and policies
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: AnomalyEvent — extreme RSI + volume anomaly", "[regime][stress]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.rsi_14 = 90.0;  // > 85 threshold
    snap.technical.rsi_valid = true;
    snap.technical.obv_normalized = 3.0;  // > 2.0 threshold
    snap.technical.obv_valid = true;
    snap.technical.adx_valid = true;
    snap.technical.adx = 10.0;
    snap.technical.ema_valid = true;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::AnomalyEvent);
    REQUIRE(result.label == RegimeLabel::Volatile);
}

TEST_CASE("Regime: ToxicFlow detection", "[regime][stress]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.microstructure.aggressive_flow = 0.85;  // > 0.75 threshold
    snap.microstructure.trade_flow_valid = true;
    snap.microstructure.spread_bps = 20.0;       // > 15 bps toxic spread
    snap.microstructure.spread_valid = true;
    snap.technical.rsi_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.adx_valid = true;
    snap.technical.adx = 10.0;
    snap.technical.ema_valid = true;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::ToxicFlow);
    REQUIRE(result.label == RegimeLabel::Volatile);
}

TEST_CASE("Regime: LiquidityStress — wide spread + low depth", "[regime][stress]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.microstructure.spread_bps = 35.0;          // > 30 threshold
    snap.microstructure.spread_valid = true;
    snap.microstructure.liquidity_ratio = 4.0;      // > 3.0 threshold
    snap.microstructure.liquidity_valid = true;
    snap.technical.rsi_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.adx_valid = true;
    snap.technical.adx = 10.0;
    snap.technical.ema_valid = true;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::LiquidityStress);
    REQUIRE(result.label == RegimeLabel::Volatile);
}

TEST_CASE("Regime: RegimePolicy — stress regimes have restrictive policies", "[regime][policy]") {
    RegimeConfig cfg;

    auto anomaly_policy = cfg.get_policy(DetailedRegime::AnomalyEvent);
    REQUIRE(anomaly_policy.action == StressAction::HaltAll);
    REQUIRE(anomaly_policy.size_cap == 0.0);
    REQUIRE(!anomaly_policy.allow_new_entries);

    auto toxic_policy = cfg.get_policy(DetailedRegime::ToxicFlow);
    REQUIRE(toxic_policy.action == StressAction::BlockEntry);
    REQUIRE(!toxic_policy.allow_new_entries);
    REQUIRE(toxic_policy.allow_exits);

    auto chop_policy = cfg.get_policy(DetailedRegime::Chop);
    REQUIRE(chop_policy.action == StressAction::ReduceSize);
    REQUIRE(chop_policy.size_cap < 1.0);
    REQUIRE(chop_policy.threshold_multiplier > 1.0);

    auto uptrend_policy = cfg.get_policy(DetailedRegime::StrongUptrend);
    REQUIRE(uptrend_policy.action == StressAction::Allow);
    REQUIRE(uptrend_policy.allow_new_entries);
    REQUIRE(uptrend_policy.size_cap == 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 6: Boundary / near-threshold transitions
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: ADX exactly at strong threshold", "[regime][boundary]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.ema_20 = 51000.0;
    snap.technical.ema_50 = 49000.0;
    snap.technical.ema_valid = true;
    snap.technical.adx = 30.0;  // Exactly at default threshold
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 60.0;
    snap.technical.rsi_valid = true;

    auto result = engine.classify(snap);

    // ADX=30 is the boundary — exact behavior depends on >= vs >
    // Either Strong or Weak uptrend is acceptable, but must not crash
    REQUIRE((result.detailed == DetailedRegime::StrongUptrend ||
             result.detailed == DetailedRegime::WeakUptrend));
}

TEST_CASE("Regime: ADX just below chop threshold stays Chop", "[regime][boundary]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.adx = 17.99;  // Just below chop_adx_max=18
    snap.technical.adx_valid = true;
    snap.technical.ema_20 = 50000.0;
    snap.technical.ema_50 = 50000.0;
    snap.technical.ema_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;

    auto result = engine.classify(snap);
    REQUIRE(result.detailed == DetailedRegime::Chop);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 7: Degraded data handling
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: all indicators invalid produces Undefined", "[regime][degraded]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.ema_valid = false;
    snap.technical.adx_valid = false;
    snap.technical.rsi_valid = false;
    snap.technical.bb_valid = false;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::Undefined);
    REQUIRE(result.label == RegimeLabel::Unclear);
}

TEST_CASE("Regime: partial data degrades gracefully", "[regime][degraded]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.rsi_14 = 60.0;
    snap.technical.rsi_valid = true;
    // All other indicators invalid
    snap.technical.ema_valid = false;
    snap.technical.adx_valid = false;
    snap.technical.bb_valid = false;

    auto result = engine.classify(snap);

    // Should produce some regime (not crash) with lower confidence
    REQUIRE(result.confidence >= 0.0);
    REQUIRE(result.confidence <= 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 8: Determinism
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: identical inputs produce identical outputs", "[regime][determinism]") {
    auto [logger1, clk1, metrics1] = make_deps();
    auto [logger2, clk2, metrics2] = make_deps();

    RuleBasedRegimeEngine engine1(logger1, clk1, metrics1);
    RuleBasedRegimeEngine engine2(logger2, clk2, metrics2);

    auto snap = make_strong_uptrend();

    auto r1 = engine1.classify(snap);
    auto r2 = engine2.classify(snap);

    REQUIRE(r1.detailed == r2.detailed);
    REQUIRE(r1.label == r2.label);
    REQUIRE_THAT(r1.confidence, Catch::Matchers::WithinAbs(r2.confidence, 1e-9));
    REQUIRE_THAT(r1.stability, Catch::Matchers::WithinAbs(r2.stability, 1e-9));
}

TEST_CASE("Regime: repeated classification is stable", "[regime][determinism]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_strong_uptrend();

    auto r1 = engine.classify(snap);
    auto r2 = engine.classify(snap);
    auto r3 = engine.classify(snap);

    REQUIRE(r1.detailed == r2.detailed);
    REQUIRE(r2.detailed == r3.detailed);
    // Stability should increase for same regime
    REQUIRE(r3.stability >= r1.stability);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 9: VolatilityExpansion
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: VolatilityExpansion — high BB bandwidth + ATR", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap = make_snapshot();
    snap.technical.bb_bandwidth = 0.10;  // > 0.06
    snap.technical.bb_valid = true;
    snap.technical.atr_14_normalized = 0.05;  // > 0.02
    snap.technical.atr_valid = true;
    snap.technical.adx = 25.0;
    snap.technical.adx_valid = true;
    snap.technical.ema_valid = true;
    snap.technical.rsi_valid = true;
    snap.technical.rsi_14 = 50.0;

    auto result = engine.classify(snap);

    REQUIRE(result.detailed == DetailedRegime::VolatilityExpansion);
    REQUIRE(result.label == RegimeLabel::Volatile);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 10: current_regime caching
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regime: current_regime returns last classification", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    // Before any classification
    auto before = engine.current_regime(Symbol("BTCUSDT"));
    REQUIRE(!before.has_value());

    // After classification
    engine.classify(make_strong_uptrend());
    auto after = engine.current_regime(Symbol("BTCUSDT"));
    REQUIRE(after.has_value());
    REQUIRE(after->detailed == DetailedRegime::StrongUptrend);
}

TEST_CASE("Regime: current_regime is symbol-specific", "[regime]") {
    auto [logger, clk, metrics] = make_deps();
    RuleBasedRegimeEngine engine(logger, clk, metrics);

    auto snap_btc = make_strong_uptrend();
    snap_btc.symbol = Symbol("BTCUSDT");
    engine.classify(snap_btc);

    auto snap_eth = make_chop();
    snap_eth.symbol = Symbol("ETHUSDT");
    engine.classify(snap_eth);

    auto btc_regime = engine.current_regime(Symbol("BTCUSDT"));
    auto eth_regime = engine.current_regime(Symbol("ETHUSDT"));
    auto unknown = engine.current_regime(Symbol("XYZUSDT"));

    REQUIRE(btc_regime.has_value());
    REQUIRE(eth_regime.has_value());
    REQUIRE(!unknown.has_value());
    REQUIRE(btc_regime->detailed == DetailedRegime::StrongUptrend);
    REQUIRE(eth_regime->detailed == DetailedRegime::Chop);
}
