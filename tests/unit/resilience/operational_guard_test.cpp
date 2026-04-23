/**
 * @file operational_guard_test.cpp
 * @brief Unit tests for OperationalGuard (Phase 7)
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "resilience/operational_guard.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::resilience;

namespace {

class TestClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    void advance(int64_t ns) { now_ns_ += ns; }
    void set(int64_t ns) { now_ns_ = ns; }
private:
    int64_t now_ns_{1'000'000'000'000LL};
};

} // namespace

TEST_CASE("OperationalGuard: normal state initially", "[guard]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuard guard({}, logger, clk);

    auto assessment = guard.assess();
    REQUIRE(assessment.verdict == GuardVerdict::Normal);
    REQUIRE(assessment.size_multiplier == 1.0);
}

TEST_CASE("OperationalGuard: consecutive failures trigger reduce-risk", "[guard][reduce]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuardConfig cfg;
    cfg.consecutive_failures_to_reduce = 3;
    cfg.reduced_size_multiplier = 0.5;
    OperationalGuard guard(cfg, logger, clk);

    guard.record_order_result(false, "rejected");
    guard.record_order_result(false, "rejected");
    guard.record_order_result(false, "rejected");

    auto assessment = guard.assess();
    REQUIRE(assessment.verdict == GuardVerdict::ReduceRisk);
    CHECK(assessment.size_multiplier == 0.5);
    CHECK(assessment.reason == ReasonCode::OpAutoReduceRisk);
}

TEST_CASE("OperationalGuard: reject rate breaker", "[guard][reject_rate]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuardConfig cfg;
    cfg.reject_rate_threshold_pct = 25.0;
    cfg.reject_window_orders = 10;
    OperationalGuard guard(cfg, logger, clk);

    // 4 out of 10 = 40% reject rate
    for (int i = 0; i < 6; i++) guard.record_order_result(true);
    for (int i = 0; i < 4; i++) guard.record_order_result(false, "rejected");

    auto assessment = guard.assess();
    REQUIRE(assessment.verdict == GuardVerdict::StopEntries);
    CHECK(assessment.reason == ReasonCode::OpRejectRateBreaker);
    CHECK(assessment.operator_alert);
}

TEST_CASE("OperationalGuard: state divergence escalation", "[guard][divergence]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuardConfig cfg;
    cfg.position_divergence_pct = 1.0;
    cfg.divergence_checks_before_halt = 3;
    OperationalGuard guard(cfg, logger, clk);

    // 3x divergence detected
    guard.record_position_check(Symbol("BTCUSDT"), 0.01, 0.02);  // 50% divergence
    guard.record_position_check(Symbol("BTCUSDT"), 0.01, 0.02);
    guard.record_position_check(Symbol("BTCUSDT"), 0.01, 0.02);

    auto assessment = guard.assess();
    REQUIRE(assessment.verdict == GuardVerdict::StopEntries);
    CHECK(assessment.reason == ReasonCode::OpStateDivergence);
}

TEST_CASE("OperationalGuard: operator halt/resume", "[guard][operator]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuard guard({}, logger, clk);

    guard.operator_halt("maintenance");

    auto assessment = guard.assess();
    REQUIRE(assessment.verdict == GuardVerdict::HaltTrading);
    CHECK(assessment.reason == ReasonCode::OpOperatorHalt);
    CHECK(assessment.size_multiplier == 0.0);

    guard.operator_resume();
    assessment = guard.assess();
    CHECK(assessment.verdict == GuardVerdict::Normal);
}

TEST_CASE("OperationalGuard: success resets consecutive failures", "[guard][reset]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuardConfig cfg;
    cfg.consecutive_failures_to_reduce = 3;
    OperationalGuard guard(cfg, logger, clk);

    guard.record_order_result(false);
    guard.record_order_result(false);
    guard.record_order_result(true);  // Reset
    guard.record_order_result(false);

    auto assessment = guard.assess();
    CHECK(assessment.verdict == GuardVerdict::Normal);
}

TEST_CASE("OperationalGuard: reset clears all state", "[guard][full_reset]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    OperationalGuardConfig cfg;
    cfg.consecutive_failures_to_reduce = 2;
    OperationalGuard guard(cfg, logger, clk);

    guard.record_order_result(false);
    guard.record_order_result(false);
    REQUIRE(guard.assess().verdict == GuardVerdict::ReduceRisk);

    guard.reset();
    CHECK(guard.assess().verdict == GuardVerdict::Normal);
}
