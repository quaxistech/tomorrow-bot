/**
 * @file config_validator.cpp
 * @brief Реализация валидатора конфигурации
 */
#include "config_validator.hpp"
#include <format>

namespace tb::config {

VoidResult ConfigValidator::validate(const AppConfig& cfg) const {
    ValidationResult result;

    validate_exchange(cfg.exchange, result);
    validate_logging(cfg.logging, result);
    validate_metrics(cfg.metrics, result);
    validate_risk(cfg.risk, result);
    validate_cross(cfg, result);

    if (!result.valid) {
        // Ошибки можно получить отдельным методом, здесь просто возвращаем код ошибки
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    return OkVoid();
}

void ConfigValidator::validate_exchange(const ExchangeConfig& cfg, ValidationResult& result) const {
    if (cfg.endpoint_rest.empty()) {
        result.add_error("exchange.endpoint_rest не может быть пустым");
    }
    if (cfg.endpoint_ws.empty()) {
        result.add_error("exchange.endpoint_ws не может быть пустым");
    }
    if (cfg.api_key_ref.empty()) {
        result.add_error("exchange.api_key_ref не может быть пустым");
    }
    if (cfg.api_secret_ref.empty()) {
        result.add_error("exchange.api_secret_ref не может быть пустым");
    }
    // Таймаут: 100ms — 30s
    if (cfg.timeout_ms < 100 || cfg.timeout_ms > 30000) {
        result.add_error(
            std::format("exchange.timeout_ms должен быть от 100 до 30000, получено: {}",
                cfg.timeout_ms)
        );
    }
}

void ConfigValidator::validate_logging(const LoggingConfig& cfg, ValidationResult& result) const {
    static constexpr std::array<std::string_view, 6> valid_levels{
        "trace", "debug", "info", "warn", "error", "critical"
    };
    bool level_ok = false;
    for (auto& lv : valid_levels) {
        if (cfg.level == lv) { level_ok = true; break; }
    }
    if (!level_ok) {
        result.add_error(
            std::format("logging.level '{}' недопустим. Допустимые: trace/debug/info/warn/error/critical",
                cfg.level)
        );
    }
}

void ConfigValidator::validate_metrics(const MetricsConfig& cfg, ValidationResult& result) const {
    if (cfg.enabled) {
        if (cfg.port < 1024 || cfg.port > 65535) {
            result.add_error(
                std::format("metrics.port {} вне допустимого диапазона [1024, 65535]", cfg.port)
            );
        }
        if (cfg.path.empty() || cfg.path[0] != '/') {
            result.add_error("metrics.path должен начинаться с '/'");
        }
    }
}

void ConfigValidator::validate_risk(const RiskConfig& cfg, ValidationResult& result) const {
    if (cfg.max_position_notional <= 0.0) {
        result.add_error("risk.max_position_notional должен быть больше 0");
    }
    if (cfg.max_daily_loss_pct <= 0.0 || cfg.max_daily_loss_pct > 100.0) {
        result.add_error("risk.max_daily_loss_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.max_drawdown_pct <= 0.0 || cfg.max_drawdown_pct > 100.0) {
        result.add_error("risk.max_drawdown_pct должен быть в диапазоне (0, 100]");
    }
    // Дневной убыток не должен превышать просадку
    if (cfg.max_daily_loss_pct > cfg.max_drawdown_pct) {
        result.add_error("risk.max_daily_loss_pct не может превышать max_drawdown_pct");
    }
}

void ConfigValidator::validate_cross(const AppConfig& cfg, ValidationResult& result) const {
    // В production обязательно включён kill-switch
    if (cfg.trading.mode == TradingMode::Production && !cfg.risk.kill_switch_enabled) {
        result.add_error("В production режиме risk.kill_switch_enabled обязателен");
    }
    // В production лог уровень не должен быть trace/debug (производительность)
    if (cfg.trading.mode == TradingMode::Production &&
        (cfg.logging.level == "trace" || cfg.logging.level == "debug")) {
        result.add_error("В production режиме уровень логирования trace/debug недопустим");
    }
}

} // namespace tb::config
