/**
 * @file config_test.cpp
 * @brief Unit тесты для валидатора конфигурации Tomorrow Bot
 * 
 * Проверяет: валидацию полей, граничные значения, межкомпонентные правила.
 */
#include <catch2/catch_test_macros.hpp>
#include "config/config_types.hpp"
#include "config/config_validator.hpp"

using namespace tb;
using namespace tb::config;

// ============================================================
// Вспомогательная функция: создаёт валидную конфигурацию по умолчанию
// ============================================================

static AppConfig make_valid_config() {
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

    cfg.risk.max_position_notional       = 1000.0;
    cfg.risk.max_daily_loss_pct          = 2.0;
    cfg.risk.max_drawdown_pct            = 5.0;
    cfg.risk.kill_switch_enabled         = true;
    cfg.risk.max_strategy_daily_loss_pct = 2.0;
    cfg.risk.max_strategy_exposure_pct   = 50.0;
    cfg.risk.max_symbol_concentration_pct = 30.0;

    cfg.trading.mode = TradingMode::Paper;

    return cfg;
}

// ============================================================
// Тесты валидной конфигурации
// ============================================================

TEST_CASE("Config: ValidConfigPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

// ============================================================
// Тесты биржевой конфигурации
// ============================================================

TEST_CASE("Config: EmptyRestEndpointFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.endpoint_rest = "";
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == TbError::ConfigValidationFailed);
}

TEST_CASE("Config: EmptyApiKeyRefFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.api_key_ref = "";
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TimeoutTooSmallFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 50; // < 100 мс
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TimeoutTooLargeFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 60000; // > 30с
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ValidTimeoutPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.exchange.timeout_ms = 100; // минимум допустимый
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

// ============================================================
// Тесты риск-параметров
// ============================================================

TEST_CASE("Config: NegativePositionNotionalFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_position_notional = -100.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: DailyLossExceedsDrawdownFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_daily_loss_pct = 10.0;
    cfg.risk.max_drawdown_pct   = 5.0; // daily > drawdown — недопустимо
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ZeroDailyLossFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.risk.max_daily_loss_pct = 0.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Тесты межкомпонентных правил
// ============================================================

TEST_CASE("Config: ProductionWithoutKillSwitchFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode           = TradingMode::Production;
    cfg.risk.kill_switch_enabled = false;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ProductionWithDebugLogFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode   = TradingMode::Production;
    cfg.logging.level  = "debug";
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ProductionWithInfoLogPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.mode   = TradingMode::Production;
    cfg.logging.level  = "info";
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

// ============================================================
// Тесты настроек логирования
// ============================================================

TEST_CASE("Config: InvalidLogLevelFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.logging.level = "verbose"; // Недопустимый уровень
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: AllValidLogLevelsPass", "[config]") {
    ConfigValidator validator;
    for (const std::string& level : {"trace", "debug", "info", "warn", "error", "critical"}) {
        auto cfg = make_valid_config();
        cfg.logging.level = level;
        auto result = validator.validate(cfg);
        REQUIRE(result.has_value());
    }
}

// ============================================================
// Тесты настроек метрик
// ============================================================

TEST_CASE("Config: MetricsInvalidPortFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.metrics.enabled = true;
    cfg.metrics.port    = 80; // < 1024
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: MetricsPathWithoutSlashFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.metrics.path = "metrics"; // Без начального '/'
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Тесты валидации trading_params
// ============================================================

TEST_CASE("Config: TradingParamsValidDefaultPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    // Defaults are valid
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

TEST_CASE("Config: TradingParamsNegativeAtrStopFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.atr_stop_multiplier = -1.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TradingParamsMaxLossOver100Fails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.max_loss_per_trade_pct = 150.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TradingParamsPartialTpBelowBreakevenFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.breakeven_atr_threshold = 2.0;
    cfg.trading_params.partial_tp_atr_threshold = 1.5; // < breakeven
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TradingParamsInvalidPartialTpFractionFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.partial_tp_fraction = 1.0; // must be < 1
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TradingParamsZeroHoldMinutesFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.max_hold_loss_minutes = 0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: TradingParamsAbsoluteHoldLessThanLossFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.max_hold_loss_minutes = 60;
    cfg.trading_params.max_hold_absolute_minutes = 30; // < loss minutes
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Тесты валидации decision
// ============================================================

TEST_CASE("Config: DecisionValidDefaultPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

TEST_CASE("Config: DecisionConvictionOverOneFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.decision.min_conviction_threshold = 1.5;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: DecisionNegativeConvictionFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.decision.min_conviction_threshold = -0.1;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: DecisionEnsembleMaxLessThanBonusFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.decision.ensemble_agreement_bonus = 0.15;
    cfg.decision.ensemble_max_bonus = 0.05; // < agreement_bonus
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: DecisionZeroTimeDecayFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.decision.time_decay_halflife_ms = 0.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Тесты валидации execution_alpha
// ============================================================

TEST_CASE("Config: ExecutionAlphaValidDefaultPasses", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    auto result = validator.validate(cfg);
    REQUIRE(result.has_value());
}

TEST_CASE("Config: ExecutionAlphaSpreadAnyLessThanPassiveFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.execution_alpha.max_spread_bps_passive = 20.0;
    cfg.execution_alpha.max_spread_bps_any = 10.0; // < passive
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ExecutionAlphaUrgencyThresholdsInvertedFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.execution_alpha.urgency_passive_threshold = 0.9;
    cfg.execution_alpha.urgency_aggressive_threshold = 0.3; // < passive
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: ExecutionAlphaAdverseSelectionOutOfRangeFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.execution_alpha.adverse_selection_threshold = 1.5;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================
// Тесты кросс-валидации
// ============================================================

TEST_CASE("Config: CrossMaxLossExceedsDailyLossFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading_params.max_loss_per_trade_pct = 5.0;
    cfg.risk.max_daily_loss_pct = 2.0;  // trade loss > daily limit
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Config: CrossZeroInitialCapitalFails", "[config]") {
    ConfigValidator validator;
    auto cfg = make_valid_config();
    cfg.trading.initial_capital = 0.0;
    auto result = validator.validate(cfg);
    REQUIRE_FALSE(result.has_value());
}
