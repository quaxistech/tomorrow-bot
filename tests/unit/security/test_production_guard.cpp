/**
 * @file test_production_guard.cpp
 * @brief Тесты ProductionGuard — защита от случайного production запуска
 */
#include <catch2/catch_test_macros.hpp>
#include "test_mocks.hpp"
#include "security/production_guard.hpp"

#include <cstdlib>

using namespace tb;
using namespace tb::test;
using namespace tb::security;

// ========== Тесты ==========

TEST_CASE("ProductionGuard: Production без подтверждения запрещён", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    ::unsetenv("TOMORROW_BOT_PRODUCTION_CONFIRM");

    auto result = guard.validate(
        TradingMode::Production,
        "production-key",
        "production-secret",
        "production-passphrase",
        "https://api.bitget.com",
        "config-hash-789");

    REQUIRE_FALSE(result.allowed);
    REQUIRE(result.detected_mode == TradingMode::Production);
    REQUIRE(result.api_keys_are_production);
}

TEST_CASE("ProductionGuard: Paper mode разрешён без подтверждения", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    ::unsetenv("TOMORROW_BOT_PRODUCTION_CONFIRM");

    auto result = guard.validate(
        TradingMode::Paper,
        "",
        "",
        "",
        "https://api.bitget.com",
        "config-hash-paper");

    REQUIRE(result.allowed);
    REQUIRE(result.detected_mode == TradingMode::Paper);
}

TEST_CASE("ProductionGuard: определение production API URL", "[security]") {
    REQUIRE(ProductionGuard::is_production_api("https://api.bitget.com"));
    REQUIRE(ProductionGuard::is_production_api("https://api.bitget.com/api/v2"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://testnet.bitget.com"));
    REQUIRE(ProductionGuard::is_production_api("http://localhost:8080"));
}
