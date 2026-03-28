/**
 * @file test_production_guard.cpp
 * @brief Тесты ProductionGuard — защита от случайного production запуска
 */
#include <catch2/catch_test_macros.hpp>
#include "security/production_guard.hpp"
#include "logging/logger.hpp"

#include <cstdlib>

using namespace tb;
using namespace tb::security;

// ========== Тестовые заглушки ==========

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

// ========== Тесты ==========

TEST_CASE("ProductionGuard: Paper режим всегда разрешён", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    auto result = guard.validate(
        TradingMode::Paper,
        "test-api-key",
        "https://testnet.binance.vision",
        "config-hash-123");

    REQUIRE(result.allowed);
    REQUIRE(result.detected_mode == TradingMode::Paper);
}

TEST_CASE("ProductionGuard: Testnet режим разрешён", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    auto result = guard.validate(
        TradingMode::Testnet,
        "testnet-key-abc",
        "https://testnet.binance.vision",
        "config-hash-456");

    REQUIRE(result.allowed);
    REQUIRE(result.detected_mode == TradingMode::Testnet);
}

TEST_CASE("ProductionGuard: Production без подтверждения запрещён", "[security]") {
    auto logger = std::make_shared<TestLogger>();
    ProductionGuard guard(logger);

    // Убеждаемся, что переменная окружения не установлена
    ::unsetenv("TOMORROW_BOT_PRODUCTION_CONFIRM");

    auto result = guard.validate(
        TradingMode::Production,
        "production-key",
        "https://api.binance.com",
        "config-hash-789");

    REQUIRE_FALSE(result.allowed);
    REQUIRE(result.detected_mode == TradingMode::Production);
    REQUIRE(result.api_keys_are_production);
}

TEST_CASE("ProductionGuard: определение production API URL", "[security]") {
    // Production URLs
    REQUIRE(ProductionGuard::is_production_api("https://api.binance.com"));
    REQUIRE(ProductionGuard::is_production_api("https://api.binance.com/api/v3"));

    // Non-production URLs (содержат "testnet")
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://testnet.binance.vision"));
    REQUIRE_FALSE(ProductionGuard::is_production_api("https://testnet.binancefuture.com"));

    // URLs без "testnet" считаются production
    REQUIRE(ProductionGuard::is_production_api("http://localhost:8080"));
}
