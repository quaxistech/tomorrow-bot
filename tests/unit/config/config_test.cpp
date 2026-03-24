/**
 * @file config_test.cpp
 * @brief Unit тесты для валидатора конфигурации Tomorrow Bot
 * 
 * Проверяет: валидацию полей, граничные значения, межкомпонентные правила.
 */
#include <gtest/gtest.h>
#include "config/config_types.hpp"
#include "config/config_validator.hpp"

using namespace tb;
using namespace tb::config;

// ============================================================
// Вспомогательная функция: создаёт валидную конфигурацию по умолчанию
// ============================================================

AppConfig make_valid_config() {
    AppConfig cfg;
    cfg.exchange.endpoint_rest  = "https://api.bitget.com";
    cfg.exchange.endpoint_ws    = "wss://ws.bitget.com/v2/ws/public";
    cfg.exchange.api_key_ref    = "BITGET_API_KEY";
    cfg.exchange.api_secret_ref = "BITGET_API_SECRET";
    cfg.exchange.timeout_ms     = 5000;

    cfg.logging.level = "info";

    cfg.metrics.enabled = true;
    cfg.metrics.port    = 9090;
    cfg.metrics.path    = "/metrics";

    cfg.health.enabled = true;
    cfg.health.port    = 8080;

    cfg.risk.max_position_notional = 1000.0;
    cfg.risk.max_daily_loss_pct    = 2.0;
    cfg.risk.max_drawdown_pct      = 5.0;
    cfg.risk.kill_switch_enabled   = true;

    cfg.trading.mode = TradingMode::Paper;

    return cfg;
}

// ============================================================
// Тесты валидной конфигурации
// ============================================================

TEST(ConfigValidatorTest, ValidConfigPasses) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    auto result = validator.validate(cfg);
    EXPECT_TRUE(result.has_value()) << "Валидная конфигурация должна пройти валидацию";
}

// ============================================================
// Тесты биржевой конфигурации
// ============================================================

TEST(ConfigValidatorTest, EmptyRestEndpointFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.endpoint_rest = "";
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value()) << "Пустой endpoint_rest должен быть отклонён";
    EXPECT_EQ(result.error(), TbError::ConfigValidationFailed);
}

TEST(ConfigValidatorTest, EmptyApiKeyRefFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.api_key_ref = "";
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigValidatorTest, TimeoutTooSmallFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 50; // < 100 мс
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value()) << "Таймаут < 100мс должен быть отклонён";
}

TEST(ConfigValidatorTest, TimeoutTooLargeFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 60000; // > 30с
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value()) << "Таймаут > 30000мс должен быть отклонён";
}

TEST(ConfigValidatorTest, ValidTimeoutPasses) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 100; // минимум допустимый
    auto result = validator.validate(cfg);
    EXPECT_TRUE(result.has_value());
}

// ============================================================
// Тесты риск-параметров
// ============================================================

TEST(ConfigValidatorTest, NegativePositionNotionalFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_position_notional = -100.0;
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value()) << "Отрицательный notional должен быть отклонён";
}

TEST(ConfigValidatorTest, DailyLossExceedsDrawdownFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_daily_loss_pct = 10.0;
    cfg.risk.max_drawdown_pct   = 5.0; // daily > drawdown — недопустимо
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value())
        << "Дневной убыток > просадки должен быть отклонён";
}

TEST(ConfigValidatorTest, ZeroDailyLossFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_daily_loss_pct = 0.0;
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// Тесты межкомпонентных правил
// ============================================================

TEST(ConfigValidatorTest, ProductionWithoutKillSwitchFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode           = TradingMode::Production;
    cfg.risk.kill_switch_enabled = false;
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value())
        << "Production без kill-switch должен быть отклонён";
}

TEST(ConfigValidatorTest, ProductionWithDebugLogFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode   = TradingMode::Production;
    cfg.logging.level  = "debug";
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value())
        << "Production с debug логом должен быть отклонён";
}

TEST(ConfigValidatorTest, ProductionWithInfoLogPasses) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode   = TradingMode::Production;
    cfg.logging.level  = "info";
    auto result = validator.validate(cfg);
    EXPECT_TRUE(result.has_value())
        << "Production с info логом должен пройти";
}

// ============================================================
// Тесты настроек логирования
// ============================================================

TEST(ConfigValidatorTest, InvalidLogLevelFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.logging.level = "verbose"; // Недопустимый уровень
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigValidatorTest, AllValidLogLevelsPass) {
    ConfigValidator validator;
    for (const std::string& level : {"trace", "debug", "info", "warn", "error", "critical"}) {
        auto cfg = make_valid_config();
        cfg.logging.level = level;
        auto result = validator.validate(cfg);
        EXPECT_TRUE(result.has_value()) << "Уровень '" << level << "' должен быть допустим";
    }
}

// ============================================================
// Тесты настроек метрик
// ============================================================

TEST(ConfigValidatorTest, MetricsInvalidPortFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.metrics.enabled = true;
    cfg.metrics.port    = 80; // < 1024
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value()) << "Порт < 1024 должен быть отклонён";
}

TEST(ConfigValidatorTest, MetricsPathWithoutSlashFails) {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.metrics.path = "metrics"; // Без начального '/'
    auto result = validator.validate(cfg);
    EXPECT_FALSE(result.has_value());
}
