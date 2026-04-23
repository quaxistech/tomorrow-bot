/**
 * @file daily_self_check_test.cpp
 * @brief Unit tests for DailySelfCheck (Phase 7)
 */

#include <catch2/catch_test_macros.hpp>

#include "self_diagnosis/daily_self_check.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::self_diagnosis;

namespace {

class TestClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    int64_t now_ns_{1'000'000'000'000LL};
};

} // namespace

TEST_CASE("DailySelfCheck: all checks pass", "[self_check]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_check("connectivity", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> { return {true, "OK"}; });
    check.register_check("balance", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> { return {true, "1500 USDT"}; });
    check.register_check("clock_sync", CheckSeverity::Warning,
        []() -> std::pair<bool, std::string> { return {true, "drift=2ms"}; });

    auto result = check.run();

    REQUIRE(result.all_critical_passed);
    REQUIRE(result.all_passed);
    CHECK(result.checks.size() == 3);
    CHECK(result.critical_failures() == 0);
    CHECK(result.warnings() == 0);
}

TEST_CASE("DailySelfCheck: critical failure blocks trading", "[self_check][critical]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_check("connectivity", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> { return {false, "Connection refused"}; });
    check.register_check("balance", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> { return {true, "1500 USDT"}; });

    auto result = check.run();

    REQUIRE_FALSE(result.all_critical_passed);
    CHECK(result.critical_failures() == 1);
}

TEST_CASE("DailySelfCheck: warning doesn't block critical", "[self_check][warning]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_check("connectivity", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> { return {true, "OK"}; });
    check.register_check("clock_sync", CheckSeverity::Warning,
        []() -> std::pair<bool, std::string> { return {false, "drift=800ms"}; });

    auto result = check.run();

    REQUIRE(result.all_critical_passed);
    REQUIRE_FALSE(result.all_passed);
    CHECK(result.warnings() == 1);
}

TEST_CASE("DailySelfCheck: exception in check → failure", "[self_check][exception]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_check("crash_check", CheckSeverity::Critical,
        []() -> std::pair<bool, std::string> {
            throw std::runtime_error("API timeout");
        });

    auto result = check.run();

    REQUIRE_FALSE(result.all_critical_passed);
    CHECK(result.checks[0].detail.find("Exception") != std::string::npos);
}

TEST_CASE("DailySelfCheck: standard checks registration", "[self_check][standard]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_standard_checks(
        []() -> std::pair<bool, std::string> { return {true, "exchange OK"}; },
        []() -> std::pair<bool, std::string> { return {true, "2000 USDT"}; },
        []() -> std::pair<bool, std::string> { return {true, "positions match"}; },
        []() -> std::pair<bool, std::string> { return {true, "WS alive"}; },
        []() -> std::pair<bool, std::string> { return {true, "drift=5ms"}; }
    );

    auto result = check.run();

    REQUIRE(result.all_critical_passed);
    CHECK(result.checks.size() == 5);
}

TEST_CASE("DailySelfCheck: last_result persists", "[self_check][last]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    DailySelfCheck check(logger, clk);

    check.register_check("basic", CheckSeverity::Info,
        []() -> std::pair<bool, std::string> { return {true, "ok"}; });

    check.run();
    auto cached = check.last_result();

    CHECK(cached.checks.size() == 1);
    CHECK(cached.all_passed);
}
