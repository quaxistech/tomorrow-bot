/// @file test_regime_ensemble.cpp
/// @brief Tests for RegimeEnsemble

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ml/regime_ensemble.hpp"

using namespace tb;
using namespace tb::ml;
using namespace tb::regime;
using Catch::Approx;

namespace {

StrategyId sid(const char* name) { return StrategyId(name); }

EnsembleOutcome make_outcome(const char* strategy, DetailedRegime regime,
                              bool profitable, double pnl_bps, int64_t ts = 1000) {
    return EnsembleOutcome{
        .strategy_id = sid(strategy),
        .regime = regime,
        .pnl_bps = pnl_bps,
        .was_profitable = profitable,
        .occurred_at = Timestamp(ts)
    };
}

} // anonymous namespace

TEST_CASE("RegimeEnsemble — uniform weights without data", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    std::vector<StrategyId> strategies = {sid("A"), sid("B"), sid("C")};

    auto result = ensemble.compute_weights(DetailedRegime::StrongUptrend, strategies, Timestamp(0));

    REQUIRE(result.weights.size() == 3);
    CHECK_FALSE(result.has_data);
    for (const auto& w : result.weights) {
        CHECK(w.weight == Approx(1.0 / 3.0).margin(0.01));
        CHECK(w.confidence == Approx(0.0));
    }
}

TEST_CASE("RegimeEnsemble — weights normalized to 1", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    std::vector<StrategyId> strategies = {sid("A"), sid("B")};

    auto result = ensemble.compute_weights(DetailedRegime::Chop, strategies, Timestamp(0));

    double sum = 0.0;
    for (const auto& w : result.weights) sum += w.weight;
    CHECK(sum == Approx(1.0).margin(0.001));
}

TEST_CASE("RegimeEnsemble — winning strategy gets higher weight", "[ml][regime_ensemble]") {
    RegimeEnsembleConfig config;
    config.min_trades_for_confidence = 5;
    RegimeEnsemble ensemble(config);

    // Strategy A wins 80% in StrongUptrend
    for (int i = 0; i < 20; ++i) {
        ensemble.record_outcome(make_outcome("A", DetailedRegime::StrongUptrend,
                                              i % 5 != 0, i % 5 != 0 ? 10.0 : -5.0, i));
    }
    // Strategy B wins 20% in StrongUptrend
    for (int i = 0; i < 20; ++i) {
        ensemble.record_outcome(make_outcome("B", DetailedRegime::StrongUptrend,
                                              i % 5 == 0, i % 5 == 0 ? 10.0 : -5.0, i));
    }

    auto result = ensemble.compute_weights(
        DetailedRegime::StrongUptrend, {sid("A"), sid("B")}, Timestamp(100));

    REQUIRE(result.weights.size() == 2);
    CHECK(result.has_data);

    double weight_a = 0, weight_b = 0;
    for (const auto& w : result.weights) {
        if (w.strategy_id.get() == "A") weight_a = w.weight;
        if (w.strategy_id.get() == "B") weight_b = w.weight;
    }
    CHECK(weight_a > weight_b);
}

TEST_CASE("RegimeEnsemble — regime-specific differentiation", "[ml][regime_ensemble]") {
    RegimeEnsembleConfig config;
    config.min_trades_for_confidence = 5;
    RegimeEnsemble ensemble(config);

    // Strategy A excels in Trending, B excels in MeanReversion
    for (int i = 0; i < 15; ++i) {
        ensemble.record_outcome(make_outcome("A", DetailedRegime::StrongUptrend,
                                              true, 10.0, i));
        ensemble.record_outcome(make_outcome("B", DetailedRegime::StrongUptrend,
                                              false, -5.0, i));
        ensemble.record_outcome(make_outcome("A", DetailedRegime::MeanReversion,
                                              false, -5.0, i));
        ensemble.record_outcome(make_outcome("B", DetailedRegime::MeanReversion,
                                              true, 10.0, i));
    }

    auto trending = ensemble.compute_weights(
        DetailedRegime::StrongUptrend, {sid("A"), sid("B")}, Timestamp(100));
    auto mean_rev = ensemble.compute_weights(
        DetailedRegime::MeanReversion, {sid("A"), sid("B")}, Timestamp(100));

    double a_trending = 0, b_trending = 0, a_meanrev = 0, b_meanrev = 0;
    for (const auto& w : trending.weights) {
        if (w.strategy_id.get() == "A") a_trending = w.weight;
        if (w.strategy_id.get() == "B") b_trending = w.weight;
    }
    for (const auto& w : mean_rev.weights) {
        if (w.strategy_id.get() == "A") a_meanrev = w.weight;
        if (w.strategy_id.get() == "B") b_meanrev = w.weight;
    }

    CHECK(a_trending > b_trending);
    CHECK(b_meanrev > a_meanrev);
}

TEST_CASE("RegimeEnsemble — minimum weight floor prevents starvation", "[ml][regime_ensemble]") {
    RegimeEnsembleConfig config;
    config.min_trades_for_confidence = 5;
    config.min_weight = 0.1;
    RegimeEnsemble ensemble(config);

    // Strategy B always loses
    for (int i = 0; i < 20; ++i) {
        ensemble.record_outcome(make_outcome("A", DetailedRegime::Chop,
                                              true, 10.0, i));
        ensemble.record_outcome(make_outcome("B", DetailedRegime::Chop,
                                              false, -5.0, i));
    }

    auto result = ensemble.compute_weights(
        DetailedRegime::Chop, {sid("A"), sid("B")}, Timestamp(100));

    for (const auto& w : result.weights) {
        CHECK(w.weight >= 0.09); // ~min_weight after normalization
    }
}

TEST_CASE("RegimeEnsemble — total outcomes tracked", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    CHECK(ensemble.total_outcomes() == 0);

    ensemble.record_outcome(make_outcome("A", DetailedRegime::Chop, true, 5.0));
    ensemble.record_outcome(make_outcome("B", DetailedRegime::Chop, false, -3.0));
    CHECK(ensemble.total_outcomes() == 2);
}

TEST_CASE("RegimeEnsemble — reset clears all data", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    ensemble.record_outcome(make_outcome("A", DetailedRegime::Chop, true, 5.0));
    ensemble.reset();

    CHECK(ensemble.total_outcomes() == 0);
    CHECK(ensemble.get_performance(sid("A"), DetailedRegime::Chop) == nullptr);
}

TEST_CASE("RegimeEnsemble — get_performance returns data", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    ensemble.record_outcome(make_outcome("A", DetailedRegime::StrongUptrend, true, 10.0));

    auto* perf = ensemble.get_performance(sid("A"), DetailedRegime::StrongUptrend);
    REQUIRE(perf != nullptr);
    CHECK(perf->trade_count == 1);
    CHECK(perf->cumulative_pnl_bps == Approx(10.0));

    auto* none = ensemble.get_performance(sid("A"), DetailedRegime::Chop);
    CHECK(none == nullptr);
}

TEST_CASE("RegimeEnsemble — empty strategy list returns empty result", "[ml][regime_ensemble]") {
    RegimeEnsemble ensemble;
    auto result = ensemble.compute_weights(
        DetailedRegime::Undefined, {}, Timestamp(0));
    CHECK(result.weights.empty());
}
