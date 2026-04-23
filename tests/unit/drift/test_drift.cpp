/**
 * @file test_drift.cpp
 * @brief Тесты DriftMonitor — PSI, KS, Page-Hinkley, ADWIN
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "drift/drift_monitor.hpp"

#include <cmath>
#include <random>

using namespace tb;
using namespace tb::drift;

TEST_CASE("DriftMonitor: register and push", "[drift]") {
    DriftMonitor monitor;
    monitor.register_stream("rsi", DriftType::Feature);
    REQUIRE(monitor.stream_count() == 1);

    monitor.push("rsi", 50.0);
    auto names = monitor.stream_names();
    REQUIRE(names.size() == 1);
    REQUIRE(names[0] == "rsi");
}

TEST_CASE("DriftMonitor: no drift for identical distributions", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 200;
    cfg.test_window = 100;
    cfg.psi_bins = 5;
    DriftMonitor monitor(cfg);
    monitor.register_stream("stable", DriftType::Feature);

    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.0);

    // Заполняем reference + test из одного распределения
    for (int i = 0; i < 300; ++i) {
        monitor.push("stable", dist(rng));
    }

    auto result = monitor.check("stable");
    REQUIRE(result.stream_name == "stable");
    REQUIRE(result.severity == DriftSeverity::None);
    REQUIRE(result.psi_value < 0.10);
}

TEST_CASE("DriftMonitor: detects shifted distribution (mean shift)", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 500;
    cfg.test_window = 200;
    cfg.psi_bins = 10;
    cfg.psi_warn = 0.10;
    cfg.psi_critical = 0.25;
    DriftMonitor monitor(cfg);
    monitor.register_stream("shifted", DriftType::Feature);

    std::mt19937 rng(42);
    std::normal_distribution<double> ref_dist(0.0, 1.0);
    std::normal_distribution<double> test_dist(3.0, 1.0);  // сильный mean shift

    // Reference
    for (int i = 0; i < 500; ++i) {
        monitor.push("shifted", ref_dist(rng));
    }
    // Test — сильный сдвиг
    for (int i = 0; i < 200; ++i) {
        monitor.push("shifted", test_dist(rng));
    }

    auto result = monitor.check("shifted");
    REQUIRE(result.psi_value > 0.25);  // should be critical
    REQUIRE(result.severity != DriftSeverity::None);
}

TEST_CASE("DriftMonitor: KS detects different distributions", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 300;
    cfg.test_window = 150;
    cfg.psi_bins = 5;
    DriftMonitor monitor(cfg);
    monitor.register_stream("ks_test", DriftType::Label);

    std::mt19937 rng(123);
    std::normal_distribution<double> ref_dist(0.0, 1.0);
    std::uniform_real_distribution<double> test_dist(-3.0, 3.0);

    for (int i = 0; i < 300; ++i) monitor.push("ks_test", ref_dist(rng));
    for (int i = 0; i < 150; ++i) monitor.push("ks_test", test_dist(rng));

    auto result = monitor.check("ks_test");
    REQUIRE(result.ks_statistic > 0.05);  // should detect difference
    REQUIRE(result.ks_p_value < 0.05);
}

TEST_CASE("DriftMonitor: check_all returns snapshot", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 100;
    cfg.test_window = 50;
    cfg.psi_bins = 5;
    DriftMonitor monitor(cfg);
    monitor.register_stream("f1", DriftType::Feature);
    monitor.register_stream("f2", DriftType::Feature);

    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.0);
    for (int i = 0; i < 150; ++i) {
        monitor.push("f1", dist(rng));
        monitor.push("f2", dist(rng));
    }

    auto snap = monitor.check_all();
    REQUIRE(snap.results.size() == 2);
}

TEST_CASE("DriftMonitor: reset_reference clears state", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 100;
    cfg.test_window = 50;
    cfg.psi_bins = 5;
    DriftMonitor monitor(cfg);
    monitor.register_stream("reset_test", DriftType::Calibration);

    std::mt19937 rng(42);
    std::normal_distribution<double> d(0, 1);
    for (int i = 0; i < 150; ++i) monitor.push("reset_test", d(rng));

    monitor.reset_reference("reset_test");

    auto result = monitor.check("reset_test");
    // After reset, test window should be empty
    REQUIRE(result.test_count == 0);
}

TEST_CASE("DriftMonitor: unknown stream returns empty result", "[drift]") {
    DriftMonitor monitor;
    auto result = monitor.check("nonexistent");
    REQUIRE(result.stream_name == "nonexistent");
    REQUIRE(result.severity == DriftSeverity::None);
}

TEST_CASE("DriftMonitor: insufficient data returns no-drift", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 1000;
    cfg.test_window = 500;
    DriftMonitor monitor(cfg);
    monitor.register_stream("small", DriftType::Feature);

    monitor.push("small", 1.0);
    monitor.push("small", 2.0);

    auto result = monitor.check("small");
    REQUIRE(result.severity == DriftSeverity::None);
}

TEST_CASE("DriftMonitor: Page-Hinkley detects step change", "[drift]") {
    DriftConfig cfg;
    cfg.reference_window = 200;
    cfg.test_window = 100;
    cfg.psi_bins = 5;
    cfg.page_hinkley_threshold = 20.0;
    cfg.page_hinkley_delta = 0.01;
    DriftMonitor monitor(cfg);
    monitor.register_stream("step", DriftType::ExecutionQuality);

    // Stable period
    for (int i = 0; i < 200; ++i) monitor.push("step", 1.0);
    // Step change
    for (int i = 0; i < 100; ++i) monitor.push("step", 5.0);

    auto result = monitor.check("step");
    REQUIRE(result.page_hinkley_alarm == true);
}
