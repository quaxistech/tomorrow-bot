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
    validate_adversarial(cfg.adversarial_defense, result);
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

void ConfigValidator::validate_adversarial(
    const AdversarialDefenseConfig& cfg, ValidationResult& result) const {
    if (cfg.spread_explosion_threshold_bps <= 0.0) {
        result.add_error("adversarial_defense.spread_explosion_threshold_bps должен быть > 0");
    }
    if (cfg.spread_normal_bps <= 0.0) {
        result.add_error("adversarial_defense.spread_normal_bps должен быть > 0");
    }
    if (cfg.min_liquidity_depth <= 0.0) {
        result.add_error("adversarial_defense.min_liquidity_depth должен быть > 0");
    }
    if (cfg.book_imbalance_threshold <= 0.0 || cfg.book_imbalance_threshold > 1.0) {
        result.add_error("adversarial_defense.book_imbalance_threshold должен быть в диапазоне (0, 1]");
    }
    if (cfg.book_instability_threshold < 0.0 || cfg.book_instability_threshold >= 1.0) {
        result.add_error("adversarial_defense.book_instability_threshold должен быть в диапазоне [0, 1)");
    }
    if (cfg.toxic_flow_ratio_threshold <= 1.0) {
        result.add_error("adversarial_defense.toxic_flow_ratio_threshold должен быть > 1.0");
    }
    if (cfg.aggressive_flow_threshold <= 0.5 || cfg.aggressive_flow_threshold >= 1.0) {
        result.add_error("adversarial_defense.aggressive_flow_threshold должен быть в диапазоне (0.5, 1.0)");
    }
    if (cfg.vpin_toxic_threshold <= 0.0 || cfg.vpin_toxic_threshold > 1.0) {
        result.add_error("adversarial_defense.vpin_toxic_threshold должен быть в диапазоне (0, 1]");
    }
    if (cfg.cooldown_duration_ms <= 0 || cfg.post_shock_cooldown_ms <= 0) {
        result.add_error("adversarial_defense cooldown должен быть > 0");
    }
    if (cfg.max_market_data_age_ns <= 0) {
        result.add_error("adversarial_defense.max_market_data_age_ns должен быть > 0");
    }
    if (cfg.auto_cooldown_severity < 0.0 || cfg.auto_cooldown_severity > 1.0) {
        result.add_error("adversarial_defense.auto_cooldown_severity должен быть в диапазоне [0, 1]");
    }
    if (cfg.max_confidence_reduction < 0.0 || cfg.max_confidence_reduction > 1.0) {
        result.add_error("adversarial_defense.max_confidence_reduction должен быть в диапазоне [0, 1]");
    }
    if (cfg.max_threshold_expansion < 0.0) {
        result.add_error("adversarial_defense.max_threshold_expansion должен быть >= 0");
    }
    if (cfg.compound_threat_factor < 0.0 || cfg.compound_threat_factor > 1.0) {
        result.add_error("adversarial_defense.compound_threat_factor должен быть в диапазоне [0, 1]");
    }
    if (cfg.cooldown_severity_scale < 1.0) {
        result.add_error("adversarial_defense.cooldown_severity_scale должен быть >= 1.0");
    }
    if (cfg.recovery_duration_ms < 0) {
        result.add_error("adversarial_defense.recovery_duration_ms должен быть >= 0");
    }
    if (cfg.recovery_confidence_floor < 0.0 || cfg.recovery_confidence_floor > 1.0) {
        result.add_error("adversarial_defense.recovery_confidence_floor должен быть в диапазоне [0, 1]");
    }
    if (cfg.spread_velocity_threshold_bps_per_sec <= 0.0) {
        result.add_error("adversarial_defense.spread_velocity_threshold_bps_per_sec должен быть > 0");
    }
    // --- Adaptive baseline ---
    if (cfg.baseline_alpha <= 0.0 || cfg.baseline_alpha >= 1.0) {
        result.add_error("adversarial_defense.baseline_alpha должен быть в диапазоне (0, 1)");
    }
    if (cfg.baseline_warmup_ticks < 1) {
        result.add_error("adversarial_defense.baseline_warmup_ticks должен быть >= 1");
    }
    if (cfg.z_score_spread_threshold <= 0.0) {
        result.add_error("adversarial_defense.z_score_spread_threshold должен быть > 0");
    }
    if (cfg.z_score_depth_threshold <= 0.0) {
        result.add_error("adversarial_defense.z_score_depth_threshold должен быть > 0");
    }
    if (cfg.z_score_ratio_threshold <= 0.0) {
        result.add_error("adversarial_defense.z_score_ratio_threshold должен быть > 0");
    }
    if (cfg.baseline_stale_reset_ms <= 0) {
        result.add_error("adversarial_defense.baseline_stale_reset_ms должен быть > 0");
    }
    // --- Threat memory ---
    if (cfg.threat_memory_alpha <= 0.0 || cfg.threat_memory_alpha >= 1.0) {
        result.add_error("adversarial_defense.threat_memory_alpha должен быть в диапазоне (0, 1)");
    }
    if (cfg.threat_memory_residual_factor < 0.0 || cfg.threat_memory_residual_factor > 1.0) {
        result.add_error("adversarial_defense.threat_memory_residual_factor должен быть в [0, 1]");
    }
    if (cfg.threat_escalation_ticks < 1) {
        result.add_error("adversarial_defense.threat_escalation_ticks должен быть >= 1");
    }
    if (cfg.threat_escalation_boost < 0.0) {
        result.add_error("adversarial_defense.threat_escalation_boost должен быть >= 0");
    }
    // --- Depth asymmetry ---
    if (cfg.depth_asymmetry_threshold <= 0.0 || cfg.depth_asymmetry_threshold >= 1.0) {
        result.add_error("adversarial_defense.depth_asymmetry_threshold должен быть в (0, 1)");
    }
    // --- Cross-signal amplification ---
    if (cfg.cross_signal_amplification < 0.0) {
        result.add_error("adversarial_defense.cross_signal_amplification должен быть >= 0");
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
