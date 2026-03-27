#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "logging/logger.hpp"
#include "ml/entropy_filter.hpp"
#include "ml/liquidation_cascade.hpp"
#include "ml/correlation_monitor.hpp"
#include "ml/microstructure_fingerprint.hpp"
#include "ml/bayesian_adapter.hpp"
#include "ml/thompson_sampler.hpp"
#include "features/feature_snapshot.hpp"

#include <vector>

namespace {
std::shared_ptr<tb::logging::ILogger> mk_logger() {
    return tb::logging::create_console_logger(tb::logging::LogLevel::Error);
}
}

TEST_CASE("EntropyFilter: entropy bounded and status usable") {
    tb::ml::EntropyFilter f(tb::ml::EntropyConfig{}, mk_logger());
    for (int i = 0; i < 80; ++i) {
        double p = 100.0 + (i % 7) * 0.2;
        f.on_tick(p, 1000.0 + i, 5.0 + (i % 3), 0.5);
    }
    const auto r = f.compute();
    REQUIRE(r.composite_entropy >= 0.0);
    REQUIRE(r.composite_entropy <= 1.0);
    REQUIRE(r.signal_quality >= 0.0);
    REQUIRE(r.signal_quality <= 1.0);
    REQUIRE(r.component_status.is_usable());
}

TEST_CASE("CascadeDetector: probability bounded") {
    tb::ml::LiquidationCascadeDetector d(tb::ml::CascadeConfig{}, mk_logger());
    for (int i = 0; i < 50; ++i) {
        double p = 100.0 + 0.1 * i;
        d.on_tick(p, 1000.0 + i * 10.0, 20000.0 - i * 10.0, 22000.0 - i * 10.0);
    }
    const auto s = d.evaluate();
    REQUIRE(s.probability >= 0.0);
    REQUIRE(s.probability <= 1.0);
    REQUIRE(s.component_status.is_usable());
}

TEST_CASE("CorrelationMonitor: correlation bounded or invalid snapshots") {
    tb::ml::CorrelationConfig cfg;
    cfg.reference_assets = {"BTCUSDT"};
    tb::ml::CorrelationMonitor c(cfg, mk_logger());

    for (int i = 0; i < 150; ++i) {
        double p = 100.0 + i * 0.1;
        c.on_primary_tick(p);
        c.on_reference_tick("BTCUSDT", 50000.0 + i * 50.0);
    }

    const auto r = c.evaluate();
    REQUIRE(r.risk_multiplier >= 0.0);
    REQUIRE(r.risk_multiplier <= 1.0);
}

TEST_CASE("MicrostructureFingerprinter: edge is bounded") {
    tb::ml::MicrostructureFingerprinter fp(tb::ml::FingerprintConfig{}, mk_logger());
    tb::features::FeatureSnapshot s{};
    s.microstructure.spread_bps = 7.5;
    s.microstructure.book_imbalance_5 = 0.2;
    s.microstructure.aggressive_flow = 0.7;
    s.technical.atr_14_normalized = 0.01;
    s.microstructure.liquidity_ratio = 1.2;

    auto f = fp.create_fingerprint(s);
    for (int i = 0; i < 50; ++i) {
        fp.record_outcome(f, (i % 3 == 0) ? 0.005 : -0.002);
    }
    const double edge = fp.predict_edge(f);
    REQUIRE(edge >= -1.0);
    REQUIRE(edge <= 1.0);
}

TEST_CASE("BayesianAdapter: produces bounded adapted values and confidence") {
    tb::ml::BayesianAdapter b(tb::ml::BayesianConfig{}, mk_logger());
    tb::ml::BayesianParameter p;
    p.name = "conviction_threshold";
    p.prior_mean = 0.3;
    p.prior_variance = 0.05;
    p.min_value = 0.1;
    p.max_value = 0.7;
    b.register_parameter("global", p);

    tb::ml::ParameterObservation o;
    o.regime = tb::regime::DetailedRegime::WeakUptrend;
    o.reward = 0.2;
    for (int i = 0; i < 40; ++i) b.record_observation("global", o);

    double v = b.get_adapted_value("global", "conviction_threshold", tb::regime::DetailedRegime::WeakUptrend);
    REQUIRE(v >= 0.1);
    REQUIRE(v <= 0.7);
    REQUIRE(b.get_confidence("global", "conviction_threshold") >= 0.0);
}

TEST_CASE("ThompsonSampler: returns valid action and bounded arm means") {
    tb::ml::ThompsonSampler t(tb::ml::ThompsonConfig{}, mk_logger());
    for (int i = 0; i < 60; ++i) {
        auto a = t.select_action();
        double reward = (i % 2 == 0) ? 0.2 : -0.1;
        t.record_reward(a, reward);
    }
    auto arms = t.get_arms();
    REQUIRE(!arms.empty());
    for (const auto& arm : arms) {
        const double mean = arm.alpha / (arm.alpha + arm.beta);
        REQUIRE(mean >= 0.0);
        REQUIRE(mean <= 1.0);
    }
}
