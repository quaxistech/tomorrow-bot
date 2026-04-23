/**
 * @file test_validation.cpp
 * @brief Тесты Walk-Forward и CPCV validation engine
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "validation/validation_engine.hpp"

using namespace tb::validation;

TEST_CASE("WalkForward: generates correct splits", "[validation]") {
    WalkForwardConfig cfg;
    cfg.train_size = 100;
    cfg.test_size = 50;
    cfg.step_size = 50;
    cfg.purge_gap = 10;
    cfg.embargo_gap = 10;

    auto splits = ValidationEngine::generate_walk_forward_splits(300, cfg);
    REQUIRE(!splits.empty());

    for (const auto& s : splits) {
        REQUIRE(s.train_end - s.train_start == 100);
        REQUIRE(s.test_end - s.test_start == 50);
        // Purge gap between train end and test start
        REQUIRE(s.test_start >= s.train_end + 10);
    }
}

TEST_CASE("WalkForward: insufficient data returns empty", "[validation]") {
    WalkForwardConfig cfg;
    cfg.train_size = 1000;
    cfg.test_size = 500;
    cfg.purge_gap = 50;

    auto splits = ValidationEngine::generate_walk_forward_splits(100, cfg);
    REQUIRE(splits.empty());
}

TEST_CASE("WalkForward: is_sufficient check", "[validation]") {
    WalkForwardConfig cfg;
    cfg.train_size = 100;
    cfg.test_size = 50;
    cfg.purge_gap = 10;

    REQUIRE(ValidationEngine::is_sufficient_for_walk_forward(160, cfg));
    REQUIRE_FALSE(ValidationEngine::is_sufficient_for_walk_forward(150, cfg));
}

TEST_CASE("WalkForward: run produces valid report", "[validation]") {
    WalkForwardConfig cfg;
    cfg.train_size = 100;
    cfg.test_size = 50;
    cfg.step_size = 50;
    cfg.purge_gap = 5;
    cfg.embargo_gap = 5;

    auto evaluator = [](const DataSplit& split) -> FoldResult {
        FoldResult r;
        r.metric_value = 10.0;  // mock: constant return
        r.hit_rate = 0.55;
        r.trade_count = 20;
        return r;
    };

    auto report = ValidationEngine::run_walk_forward(500, cfg, evaluator);
    REQUIRE(report.is_valid);
    REQUIRE(report.method == "walk_forward");
    REQUIRE(report.total_folds > 0);
    REQUIRE_THAT(report.mean_metric, Catch::Matchers::WithinAbs(10.0, 0.01));
    REQUIRE_THAT(report.std_metric, Catch::Matchers::WithinAbs(0.0, 0.01));
}

TEST_CASE("CPCV: generates splits", "[validation]") {
    CPCVConfig cfg;
    cfg.n_groups = 5;
    cfg.n_test_groups = 1;
    cfg.purge_gap = 5;
    cfg.embargo_gap = 5;

    auto splits = ValidationEngine::generate_cpcv_splits(1000, cfg);
    REQUIRE(!splits.empty());
    // C(5,1) = 5 combinations
    REQUIRE(splits.size() == 5);

    for (const auto& s : splits) {
        REQUIRE(s.test_end > s.test_start);
        REQUIRE(s.train_end > s.train_start);
    }
}

TEST_CASE("CPCV: run produces valid report", "[validation]") {
    CPCVConfig cfg;
    cfg.n_groups = 4;
    cfg.n_test_groups = 1;
    cfg.purge_gap = 2;
    cfg.embargo_gap = 2;

    auto evaluator = [](const DataSplit& split) -> FoldResult {
        FoldResult r;
        r.metric_value = 5.0 + static_cast<double>(split.test_start % 3);
        r.trade_count = 10;
        return r;
    };

    auto report = ValidationEngine::run_cpcv(1000, cfg, evaluator);
    REQUIRE(report.is_valid);
    REQUIRE(report.method == "cpcv");
    REQUIRE(report.total_folds == 4);  // C(4,1) = 4
}

TEST_CASE("CPCV: insufficient data returns invalid", "[validation]") {
    CPCVConfig cfg;
    cfg.n_groups = 10;
    cfg.n_test_groups = 2;
    cfg.purge_gap = 100;
    cfg.embargo_gap = 100;

    auto report = ValidationEngine::run_cpcv(50, cfg, [](const DataSplit&) {
        return FoldResult{};
    });
    REQUIRE_FALSE(report.is_valid);
}

TEST_CASE("CPCV: invalid config (test_groups >= groups)", "[validation]") {
    CPCVConfig cfg;
    cfg.n_groups = 3;
    cfg.n_test_groups = 3;

    auto splits = ValidationEngine::generate_cpcv_splits(1000, cfg);
    REQUIRE(splits.empty());
}

TEST_CASE("WalkForward: purge gap separates train from test", "[validation]") {
    WalkForwardConfig cfg;
    cfg.train_size = 100;
    cfg.test_size = 50;
    cfg.step_size = 50;
    cfg.purge_gap = 20;
    cfg.embargo_gap = 10;

    auto splits = ValidationEngine::generate_walk_forward_splits(400, cfg);
    for (const auto& s : splits) {
        // No overlap: test_start must be >= train_end + purge_gap
        REQUIRE(s.test_start >= s.train_end + cfg.purge_gap);
    }
}
