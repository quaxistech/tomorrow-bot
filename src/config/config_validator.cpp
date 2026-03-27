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
    validate_opportunity_cost(cfg.opportunity_cost, result);
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
    // --- v4: Percentile scoring ---
    if (cfg.percentile_window_size < 10) {
        result.add_error("adversarial_defense.percentile_window_size должен быть >= 10");
    }
    if (cfg.percentile_severity_threshold <= 0.0 || cfg.percentile_severity_threshold >= 1.0) {
        result.add_error("adversarial_defense.percentile_severity_threshold должен быть в (0, 1)");
    }
    // --- v4: Correlation matrix ---
    if (cfg.correlation_alpha <= 0.0 || cfg.correlation_alpha >= 1.0) {
        result.add_error("adversarial_defense.correlation_alpha должен быть в (0, 1)");
    }
    if (cfg.correlation_breakdown_threshold <= 0.0) {
        result.add_error("adversarial_defense.correlation_breakdown_threshold должен быть > 0");
    }
    // --- v4: Multi-timeframe ---
    if (cfg.baseline_halflife_fast_ms <= 0.0) {
        result.add_error("adversarial_defense.baseline_halflife_fast_ms должен быть > 0");
    }
    if (cfg.baseline_halflife_medium_ms <= cfg.baseline_halflife_fast_ms) {
        result.add_error("adversarial_defense.baseline_halflife_medium_ms должен быть > fast");
    }
    if (cfg.baseline_halflife_slow_ms <= cfg.baseline_halflife_medium_ms) {
        result.add_error("adversarial_defense.baseline_halflife_slow_ms должен быть > medium");
    }
    if (cfg.timeframe_divergence_threshold <= 0.0) {
        result.add_error("adversarial_defense.timeframe_divergence_threshold должен быть > 0");
    }
    // --- v4: Hysteresis ---
    if (cfg.hysteresis_enter_severity <= 0.0 || cfg.hysteresis_enter_severity >= 1.0) {
        result.add_error("adversarial_defense.hysteresis_enter_severity должен быть в (0, 1)");
    }
    if (cfg.hysteresis_exit_severity < 0.0 || cfg.hysteresis_exit_severity >= cfg.hysteresis_enter_severity) {
        result.add_error("adversarial_defense.hysteresis_exit_severity должен быть в [0, enter)");
    }
    if (cfg.hysteresis_confidence_penalty < 0.0 || cfg.hysteresis_confidence_penalty > 1.0) {
        result.add_error("adversarial_defense.hysteresis_confidence_penalty должен быть в [0, 1]");
    }
    // --- v4: Event sourcing ---
    if (cfg.audit_log_max_size < 0) {
        result.add_error("adversarial_defense.audit_log_max_size должен быть >= 0");
    }
}

void ConfigValidator::validate_opportunity_cost(
    const OpportunityCostConfig& cfg, ValidationResult& result) const {
    if (cfg.execute_min_net_bps < cfg.min_net_expected_bps) {
        result.add_error("opportunity_cost.execute_min_net_bps должен быть >= min_net_expected_bps");
    }
    if (cfg.high_exposure_threshold <= 0.0 || cfg.high_exposure_threshold > 1.0) {
        result.add_error("opportunity_cost.high_exposure_threshold должен быть в (0, 1]");
    }
    if (cfg.high_exposure_min_conviction <= 0.0 || cfg.high_exposure_min_conviction > 1.0) {
        result.add_error("opportunity_cost.high_exposure_min_conviction должен быть в (0, 1]");
    }
    if (cfg.max_symbol_concentration <= 0.0 || cfg.max_symbol_concentration > 1.0) {
        result.add_error("opportunity_cost.max_symbol_concentration должен быть в (0, 1]");
    }
    if (cfg.max_strategy_concentration <= 0.0 || cfg.max_strategy_concentration > 1.0) {
        result.add_error("opportunity_cost.max_strategy_concentration должен быть в (0, 1]");
    }
    if (cfg.capital_exhaustion_threshold <= 0.0 || cfg.capital_exhaustion_threshold > 1.0) {
        result.add_error("opportunity_cost.capital_exhaustion_threshold должен быть в (0, 1]");
    }
    // Веса должны быть положительными и давать в сумме 1.0 (±допуск)
    double total_weight = cfg.weight_conviction + cfg.weight_net_edge +
                          cfg.weight_capital_efficiency + cfg.weight_urgency;
    if (std::abs(total_weight - 1.0) > 0.01) {
        result.add_error(
            std::format("opportunity_cost weights должны давать сумму 1.0, получено: {:.3f}",
                total_weight));
    }
    if (cfg.weight_conviction < 0.0 || cfg.weight_net_edge < 0.0 ||
        cfg.weight_capital_efficiency < 0.0 || cfg.weight_urgency < 0.0) {
        result.add_error("opportunity_cost weights не могут быть отрицательными");
    }
    if (cfg.conviction_to_bps_scale <= 0.0) {
        result.add_error("opportunity_cost.conviction_to_bps_scale должен быть > 0");
    }
    if (cfg.upgrade_min_edge_advantage_bps < 0.0) {
        result.add_error("opportunity_cost.upgrade_min_edge_advantage_bps должен быть >= 0");
    }
    if (cfg.drawdown_penalty_scale < 0.0) {
        result.add_error("opportunity_cost.drawdown_penalty_scale должен быть >= 0");
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
