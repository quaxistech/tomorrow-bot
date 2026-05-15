/// @file test_calibration.cpp
/// @brief Tests for PlattCalibrator and IsotonicCalibrator

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ml/calibration.hpp"
#include <cmath>

using namespace tb::ml;
using Catch::Approx;

TEST_CASE("PlattCalibrator — insufficient data does not fit", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 20;
    PlattCalibrator cal(config);

    for (int i = 0; i < 10; ++i) {
        cal.add_sample(static_cast<double>(i) * 0.1, i > 5);
    }
    cal.fit();
    CHECK_FALSE(cal.is_fitted());
}

TEST_CASE("PlattCalibrator — calibrate returns sigmoid output", "[ml][calibration]") {
    PlattCalibrator cal;

    // Without fitting, default A=0, B=0: calibrate(x) = 1/(1+exp(0)) = 0.5
    CHECK(cal.calibrate(0.5) == Approx(0.5).margin(0.01));
    CHECK(cal.calibrate(100.0) == Approx(0.5).margin(0.01));
}

TEST_CASE("PlattCalibrator — monotonic after fitting", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 30;
    PlattCalibrator cal(config);

    // Clear separation: low scores → negative, high scores → positive
    for (int i = 0; i < 50; ++i) {
        double score = static_cast<double>(i) / 50.0;
        bool label = (i >= 25); // Bottom half negative, top half positive
        cal.add_sample(score, label);
    }
    cal.fit();
    REQUIRE(cal.is_fitted());

    // After Platt fitting, higher raw scores should map to higher probabilities
    double p_low = cal.calibrate(0.1);
    double p_mid = cal.calibrate(0.5);
    double p_high = cal.calibrate(0.9);

    CHECK(p_low < p_mid);
    CHECK(p_mid < p_high);
    CHECK(p_low >= 0.0);
    CHECK(p_high <= 1.0);
}

TEST_CASE("PlattCalibrator — brier score decreases after fitting", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 30;
    PlattCalibrator cal(config);

    for (int i = 0; i < 60; ++i) {
        double score = static_cast<double>(i) / 60.0;
        bool label = (i >= 30);
        cal.add_sample(score, label);
    }

    double brier_before = cal.brier_score(); // Uses default A=0, B=0

    cal.fit();
    REQUIRE(cal.is_fitted());

    double brier_after = cal.brier_score();
    CHECK(brier_after < brier_before);
}

TEST_CASE("PlattCalibrator — max_samples enforced", "[ml][calibration]") {
    CalibrationConfig config;
    config.max_samples = 50;
    PlattCalibrator cal(config);

    for (int i = 0; i < 100; ++i) {
        cal.add_sample(static_cast<double>(i) * 0.01, i % 2 == 0);
    }
    CHECK(cal.sample_count() == 50);
}

TEST_CASE("IsotonicCalibrator — insufficient data does not fit", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 20;
    IsotonicCalibrator cal(config);

    for (int i = 0; i < 10; ++i) {
        cal.add_sample(static_cast<double>(i) * 0.1, i > 5);
    }
    cal.fit();
    CHECK_FALSE(cal.is_fitted());
}

TEST_CASE("IsotonicCalibrator — calibrate returns 0.5 when unfitted", "[ml][calibration]") {
    IsotonicCalibrator cal;
    CHECK(cal.calibrate(0.5) == Approx(0.5));
}

TEST_CASE("IsotonicCalibrator — monotonic after fitting", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 30;
    config.isotonic_bins = 10;
    IsotonicCalibrator cal(config);

    // Clear separation: low scores → negative, high scores → positive
    for (int i = 0; i < 60; ++i) {
        double score = static_cast<double>(i) / 60.0;
        bool label = (i >= 30);
        cal.add_sample(score, label);
    }
    cal.fit();
    REQUIRE(cal.is_fitted());

    // After isotonic fitting, the output should be monotonically non-decreasing
    double prev = cal.calibrate(0.0);
    for (int i = 1; i <= 100; ++i) {
        double score = static_cast<double>(i) / 100.0;
        double p = cal.calibrate(score);
        CHECK(p >= prev - 1e-9); // Allow tiny float imprecision
        prev = p;
    }
}

TEST_CASE("IsotonicCalibrator — output probabilities in [0, 1]", "[ml][calibration]") {
    CalibrationConfig config;
    config.min_samples = 30;
    IsotonicCalibrator cal(config);

    for (int i = 0; i < 50; ++i) {
        double score = static_cast<double>(i) / 50.0;
        cal.add_sample(score, i >= 25);
    }
    cal.fit();
    REQUIRE(cal.is_fitted());

    for (int i = -10; i <= 110; ++i) {
        double score = static_cast<double>(i) / 100.0;
        double p = cal.calibrate(score);
        CHECK(p >= 0.0);
        CHECK(p <= 1.0);
    }
}
