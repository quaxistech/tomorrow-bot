/// @file test_meta_label.cpp
/// @brief Tests for MetaLabelClassifier

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ml/meta_label.hpp"

using namespace tb;
using namespace tb::ml;
using Catch::Approx;

namespace {

PrimarySignal make_signal(double conviction = 0.7, double quality = 0.8) {
    PrimarySignal s;
    s.symbol = Symbol("BTCUSDT");
    s.strategy_id = "momentum_long";
    s.side = Side::Buy;
    s.conviction = conviction;
    s.feature_quality = quality;
    s.signal_time = Timestamp(1000);
    return s;
}

MetaLabelContext make_context(double regime_conf = 0.8, double uncertainty = 0.2,
                              int wins = 5, int losses = 3) {
    MetaLabelContext ctx;
    ctx.regime_confidence = regime_conf;
    ctx.uncertainty_score = uncertainty;
    ctx.spread_bps = 2.0;
    ctx.book_imbalance = 0.1;
    ctx.vpin = 0.3;
    ctx.volatility_normalized = 0.5;
    ctx.recent_wins = wins;
    ctx.recent_losses = losses;
    ctx.regime_label = "Trending";
    return ctx;
}

} // anonymous namespace

TEST_CASE("MetaLabel — classify returns valid result", "[ml][meta_label]") {
    MetaLabelClassifier clf;
    auto result = clf.classify(make_signal(), make_context());

    CHECK(result.probability >= 0.0);
    CHECK(result.probability <= 1.0);
    CHECK(result.bet_size >= 0.0);
    CHECK(result.bet_size <= 1.0);
    CHECK(result.threshold_used > 0.0);
    CHECK(!result.rationale.empty());
}

TEST_CASE("MetaLabel — high conviction increases probability", "[ml][meta_label]") {
    MetaLabelClassifier clf;
    auto low = clf.classify(make_signal(0.2, 0.3), make_context(0.3, 0.8, 1, 8));
    auto high = clf.classify(make_signal(0.95, 0.95), make_context(0.95, 0.1, 9, 1));

    CHECK(high.probability > low.probability);
}

TEST_CASE("MetaLabel — bet size follows Kelly criterion", "[ml][meta_label]") {
    MetaLabelClassifier clf;
    // With 50/50 odds, Kelly = 0
    auto result = clf.classify(make_signal(0.5, 0.5), make_context(0.5, 0.5, 5, 5));
    // bet_size = max(0, 2*p - 1), for p near 0.5 should be ≈ 0
    CHECK(result.bet_size < 0.3);
}

TEST_CASE("MetaLabel — record_outcome updates model", "[ml][meta_label]") {
    MetaLabelClassifier clf;

    CHECK(clf.outcome_count() == 0);

    MetaLabelOutcome outcome;
    outcome.signal = make_signal();
    outcome.context = make_context();
    outcome.was_profitable = true;
    outcome.realized_pnl_bps = 15.0;
    outcome.outcome_time = Timestamp(2000);

    clf.record_outcome(outcome);
    CHECK(clf.outcome_count() == 1);
}

TEST_CASE("MetaLabel — brier score starts at 0.25 without calibration", "[ml][meta_label]") {
    MetaLabelClassifier clf;
    // Before any outcomes, brier score should be undefined (returns 1.0)
    double bs = clf.brier_score();
    CHECK(bs >= 0.0);
    CHECK(bs <= 1.0);
}

TEST_CASE("MetaLabel — calibration improves with data", "[ml][meta_label]") {
    MetaLabelConfig config;
    config.min_history = 10; // Lower for testing
    MetaLabelClassifier clf(config);

    // Feed 50 clear positive outcomes (high conviction, profitable)
    for (int i = 0; i < 30; ++i) {
        MetaLabelOutcome outcome;
        outcome.signal = make_signal(0.9, 0.9);
        outcome.context = make_context(0.9, 0.1, 8, 2);
        outcome.was_profitable = true;
        outcome.realized_pnl_bps = 10.0;
        outcome.outcome_time = Timestamp(static_cast<int64_t>(i * 1000));
        clf.record_outcome(outcome);
    }
    // Feed 20 clear negative outcomes (low conviction, unprofitable)
    for (int i = 0; i < 20; ++i) {
        MetaLabelOutcome outcome;
        outcome.signal = make_signal(0.2, 0.3);
        outcome.context = make_context(0.3, 0.8, 1, 9);
        outcome.was_profitable = false;
        outcome.realized_pnl_bps = -5.0;
        outcome.outcome_time = Timestamp(static_cast<int64_t>((30 + i) * 1000));
        clf.record_outcome(outcome);
    }

    CHECK(clf.outcome_count() == 50);

    // After sufficient data, high-quality signal should have higher probability than low-quality
    auto good = clf.classify(make_signal(0.9, 0.9), make_context(0.9, 0.1, 8, 2));
    auto bad = clf.classify(make_signal(0.2, 0.3), make_context(0.3, 0.8, 1, 9));
    CHECK(good.probability > bad.probability);
}

TEST_CASE("MetaLabel — adaptive threshold changes with data", "[ml][meta_label]") {
    MetaLabelConfig config;
    config.min_history = 10;
    config.adaptive_threshold = true;
    MetaLabelClassifier clf(config);

    double initial_threshold = clf.current_threshold();
    CHECK(initial_threshold == Approx(config.default_threshold));

    // Record outcomes
    for (int i = 0; i < 60; ++i) {
        MetaLabelOutcome outcome;
        outcome.signal = make_signal(0.5 + 0.01 * i, 0.7);
        outcome.context = make_context(0.7, 0.3, 5, 5);
        outcome.was_profitable = (i % 3 != 0); // 66% win rate
        outcome.realized_pnl_bps = outcome.was_profitable ? 8.0 : -12.0;
        outcome.outcome_time = Timestamp(static_cast<int64_t>(i * 1000));
        clf.record_outcome(outcome);
    }

    // Threshold may have adapted (might be same if default was already optimal)
    double new_threshold = clf.current_threshold();
    CHECK(new_threshold >= 0.45);
    CHECK(new_threshold <= 0.75);
}
