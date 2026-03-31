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
    validate_trading_params(cfg.trading_params, result);
    validate_decision(cfg.decision, result);
    validate_execution_alpha(cfg.execution_alpha, result);
    validate_futures(cfg.futures, result);
    validate_cross(cfg, result);

    if (!result.valid) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    return OkVoid();
}

ValidationResult ConfigValidator::validate_detailed(const AppConfig& cfg) const {
    ValidationResult result;

    validate_exchange(cfg.exchange, result);
    validate_logging(cfg.logging, result);
    validate_metrics(cfg.metrics, result);
    validate_risk(cfg.risk, result);
    validate_adversarial(cfg.adversarial_defense, result);
    validate_opportunity_cost(cfg.opportunity_cost, result);
    validate_trading_params(cfg.trading_params, result);
    validate_decision(cfg.decision, result);
    validate_execution_alpha(cfg.execution_alpha, result);
    validate_futures(cfg.futures, result);
    validate_cross(cfg, result);

    return result;
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
    // Расширенные параметры
    if (cfg.max_strategy_daily_loss_pct <= 0.0 || cfg.max_strategy_daily_loss_pct > 100.0) {
        result.add_error("risk.max_strategy_daily_loss_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.max_strategy_exposure_pct <= 0.0 || cfg.max_strategy_exposure_pct > 100.0) {
        result.add_error("risk.max_strategy_exposure_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.max_symbol_concentration_pct <= 0.0 || cfg.max_symbol_concentration_pct > 100.0) {
        result.add_error("risk.max_symbol_concentration_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.max_same_direction_positions < 1) {
        result.add_error("risk.max_same_direction_positions должен быть >= 1");
    }
    if (cfg.stress_regime_scale <= 0.0 || cfg.stress_regime_scale > 2.0) {
        result.add_error("risk.stress_regime_scale должен быть в диапазоне (0, 2]");
    }
    if (cfg.trending_regime_scale <= 0.0 || cfg.trending_regime_scale > 3.0) {
        result.add_error("risk.trending_regime_scale должен быть в диапазоне (0, 3]");
    }
    if (cfg.chop_regime_scale <= 0.0 || cfg.chop_regime_scale > 2.0) {
        result.add_error("risk.chop_regime_scale должен быть в диапазоне (0, 2]");
    }
    if (cfg.max_trades_per_hour < 1) {
        result.add_error("risk.max_trades_per_hour должен быть >= 1");
    }
    if (cfg.min_trade_interval_sec < 0.0) {
        result.add_error("risk.min_trade_interval_sec не может быть отрицательным");
    }
    if (cfg.max_adverse_excursion_pct <= 0.0 || cfg.max_adverse_excursion_pct > 100.0) {
        result.add_error("risk.max_adverse_excursion_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.max_realized_daily_loss_pct <= 0.0 || cfg.max_realized_daily_loss_pct > 100.0) {
        result.add_error("risk.max_realized_daily_loss_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.utc_cutoff_hour < -1 || cfg.utc_cutoff_hour > 23) {
        result.add_error("risk.utc_cutoff_hour должен быть -1 (отключено) или в диапазоне [0, 23]");
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

void ConfigValidator::validate_trading_params(
    const TradingParamsConfig& cfg, ValidationResult& result) const {
    if (cfg.atr_stop_multiplier <= 0.0 || cfg.atr_stop_multiplier > 10.0) {
        result.add_error("trading_params.atr_stop_multiplier должен быть в диапазоне (0, 10]");
    }
    if (cfg.max_loss_per_trade_pct <= 0.0 || cfg.max_loss_per_trade_pct > 100.0) {
        result.add_error("trading_params.max_loss_per_trade_pct должен быть в диапазоне (0, 100]");
    }
    if (cfg.breakeven_atr_threshold <= 0.0) {
        result.add_error("trading_params.breakeven_atr_threshold должен быть > 0");
    }
    if (cfg.partial_tp_atr_threshold <= 0.0) {
        result.add_error("trading_params.partial_tp_atr_threshold должен быть > 0");
    }
    if (cfg.partial_tp_atr_threshold <= cfg.breakeven_atr_threshold) {
        result.add_error("trading_params.partial_tp_atr_threshold должен быть > breakeven_atr_threshold");
    }
    if (cfg.partial_tp_fraction <= 0.0 || cfg.partial_tp_fraction > 1.0) {
        result.add_error("trading_params.partial_tp_fraction должен быть в диапазоне (0, 1]");
    }
    if (cfg.max_hold_loss_minutes < 1 || cfg.max_hold_loss_minutes > 1440) {
        result.add_error("trading_params.max_hold_loss_minutes должен быть в диапазоне [1, 1440]");
    }
    if (cfg.max_hold_absolute_minutes < 1 || cfg.max_hold_absolute_minutes > 10080) {
        result.add_error("trading_params.max_hold_absolute_minutes должен быть в диапазоне [1, 10080]");
    }
    if (cfg.max_hold_absolute_minutes < cfg.max_hold_loss_minutes) {
        result.add_error("trading_params.max_hold_absolute_minutes не может быть < max_hold_loss_minutes");
    }
    if (cfg.order_cooldown_seconds < 0 || cfg.order_cooldown_seconds > 3600) {
        result.add_error("trading_params.order_cooldown_seconds должен быть в диапазоне [0, 3600]");
    }
    if (cfg.stop_loss_cooldown_seconds < 0 || cfg.stop_loss_cooldown_seconds > 86400) {
        result.add_error("trading_params.stop_loss_cooldown_seconds должен быть в диапазоне [0, 86400]");
    }
}

void ConfigValidator::validate_decision(
    const DecisionConfig& cfg, ValidationResult& result) const {
    if (cfg.min_conviction_threshold <= 0.0 || cfg.min_conviction_threshold >= 1.0) {
        result.add_error("decision.min_conviction_threshold должен быть в диапазоне (0, 1)");
    }
    if (cfg.conflict_dominance_threshold <= 0.0 || cfg.conflict_dominance_threshold >= 1.0) {
        result.add_error("decision.conflict_dominance_threshold должен быть в диапазоне (0, 1)");
    }
    if (cfg.time_decay_halflife_ms <= 0.0) {
        result.add_error("decision.time_decay_halflife_ms должен быть > 0");
    }
    if (cfg.ensemble_agreement_bonus < 0.0 || cfg.ensemble_agreement_bonus > 1.0) {
        result.add_error("decision.ensemble_agreement_bonus должен быть в диапазоне [0, 1]");
    }
    if (cfg.ensemble_max_bonus < 0.0 || cfg.ensemble_max_bonus > 1.0) {
        result.add_error("decision.ensemble_max_bonus должен быть в диапазоне [0, 1]");
    }
    if (cfg.ensemble_max_bonus < cfg.ensemble_agreement_bonus) {
        result.add_error("decision.ensemble_max_bonus не может быть < ensemble_agreement_bonus");
    }
    if (cfg.drawdown_boost_scale < 0.0 || cfg.drawdown_boost_scale > 1.0) {
        result.add_error("decision.drawdown_boost_scale должен быть в диапазоне [0, 1]");
    }
    if (cfg.max_acceptable_cost_bps < 0.0) {
        result.add_error("decision.max_acceptable_cost_bps должен быть >= 0");
    }
}

void ConfigValidator::validate_execution_alpha(
    const ExecutionAlphaConfig& cfg, ValidationResult& result) const {
    if (cfg.max_spread_bps_passive <= 0.0) {
        result.add_error("execution_alpha.max_spread_bps_passive должен быть > 0");
    }
    if (cfg.max_spread_bps_any <= 0.0) {
        result.add_error("execution_alpha.max_spread_bps_any должен быть > 0");
    }
    if (cfg.max_spread_bps_any < cfg.max_spread_bps_passive) {
        result.add_error("execution_alpha.max_spread_bps_any должен быть >= max_spread_bps_passive");
    }
    if (cfg.adverse_selection_threshold <= 0.0 || cfg.adverse_selection_threshold >= 1.0) {
        result.add_error("execution_alpha.adverse_selection_threshold должен быть в диапазоне (0, 1)");
    }
    if (cfg.urgency_passive_threshold < 0.0 || cfg.urgency_passive_threshold >= 1.0) {
        result.add_error("execution_alpha.urgency_passive_threshold должен быть в диапазоне [0, 1)");
    }
    if (cfg.urgency_aggressive_threshold <= cfg.urgency_passive_threshold ||
        cfg.urgency_aggressive_threshold > 1.0) {
        result.add_error("execution_alpha.urgency_aggressive_threshold должен быть в (passive_threshold, 1]");
    }
    if (cfg.large_order_slice_threshold <= 0.0 || cfg.large_order_slice_threshold >= 1.0) {
        result.add_error("execution_alpha.large_order_slice_threshold должен быть в диапазоне (0, 1)");
    }
    if (cfg.vpin_toxic_threshold <= 0.0 || cfg.vpin_toxic_threshold >= 1.0) {
        result.add_error("execution_alpha.vpin_toxic_threshold должен быть в диапазоне (0, 1)");
    }
    if (cfg.vpin_weight < 0.0 || cfg.vpin_weight > 1.0) {
        result.add_error("execution_alpha.vpin_weight должен быть в диапазоне [0, 1]");
    }
    if (cfg.limit_price_passive_bps < 0.0) {
        result.add_error("execution_alpha.limit_price_passive_bps должен быть >= 0");
    }
    if (cfg.min_fill_probability_passive < 0.0 || cfg.min_fill_probability_passive > 1.0) {
        result.add_error("execution_alpha.min_fill_probability_passive должен быть в диапазоне [0, 1]");
    }
    if (cfg.postonly_spread_threshold_bps < 0.0) {
        result.add_error("execution_alpha.postonly_spread_threshold_bps должен быть >= 0");
    }
    if (cfg.postonly_urgency_max < 0.0 || cfg.postonly_urgency_max > 1.0) {
        result.add_error("execution_alpha.postonly_urgency_max должен быть в диапазоне [0, 1]");
    }
    if (cfg.postonly_adverse_max < 0.0 || cfg.postonly_adverse_max > 1.0) {
        result.add_error("execution_alpha.postonly_adverse_max должен быть в диапазоне [0, 1]");
    }
}

void ConfigValidator::validate_cross(const AppConfig& cfg, ValidationResult& result) const {
    // В production обязательно включён kill-switch
    if (cfg.trading.mode == TradingMode::Production && !cfg.risk.kill_switch_enabled) {
        result.add_error("В production режиме risk.kill_switch_enabled обязателен");
    }
    // В production adversarial defense обязателен
    if (cfg.trading.mode == TradingMode::Production && !cfg.adversarial_defense.enabled) {
        result.add_error("В production режиме adversarial_defense.enabled обязателен");
    }
    // В production futures должны быть сконфигурированы, если включены
    if (cfg.trading.mode == TradingMode::Production && cfg.futures.enabled &&
        cfg.futures.max_leverage > 20) {
        result.add_warning(
            std::format("futures.max_leverage ({}) > 20 в production — высокий риск ликвидации",
                        cfg.futures.max_leverage));
    }
    // В production лог уровень не должен быть trace/debug (производительность)
    if (cfg.trading.mode == TradingMode::Production &&
        (cfg.logging.level == "trace" || cfg.logging.level == "debug")) {
        result.add_error("В production режиме уровень логирования trace/debug недопустим");
    }
    // Начальный капитал должен покрывать хотя бы один минимальный ордер
    if (cfg.trading.initial_capital <= 0.0) {
        result.add_error("trading.initial_capital должен быть > 0");
    }
    // Порт-коллизия: metrics и health не должны слушать на одном порту
    if (cfg.metrics.enabled && cfg.health.enabled && cfg.metrics.port == cfg.health.port) {
        result.add_error(
            std::format("metrics.port ({}) и health.port ({}) не должны совпадать",
                cfg.metrics.port, cfg.health.port));
    }
    // health.port должен быть в допустимом диапазоне
    if (cfg.health.enabled && (cfg.health.port < 1024 || cfg.health.port > 65535)) {
        result.add_error(
            std::format("health.port {} вне допустимого диапазона [1024, 65535]",
                cfg.health.port));
    }
    // max_loss_per_trade не должен превышать daily loss limit
    if (cfg.trading_params.max_loss_per_trade_pct > cfg.risk.max_daily_loss_pct) {
        result.add_error("trading_params.max_loss_per_trade_pct не может превышать risk.max_daily_loss_pct");
    }
    // Execution alpha spread limits должны быть согласованы с adversarial defense
    if (cfg.execution_alpha.max_spread_bps_any > 0.0 &&
        cfg.adversarial_defense.spread_explosion_threshold_bps > 0.0 &&
        cfg.execution_alpha.max_spread_bps_any > cfg.adversarial_defense.spread_explosion_threshold_bps) {
        result.add_error(
            "execution_alpha.max_spread_bps_any не должен превышать "
            "adversarial_defense.spread_explosion_threshold_bps");
    }
}

void ConfigValidator::validate_futures(const FuturesConfig& cfg, ValidationResult& result) const {
    if (!cfg.enabled) return; // Если фьючерсы отключены — не валидируем

    // product_type
    if (cfg.product_type != "USDT-FUTURES" && cfg.product_type != "COIN-FUTURES" &&
        cfg.product_type != "USDC-FUTURES") {
        result.add_error(
            std::format("futures.product_type '{}' неизвестен; "
                        "допустимые: USDT-FUTURES, COIN-FUTURES, USDC-FUTURES",
                        cfg.product_type));
    }

    // margin_mode
    if (cfg.margin_mode != "isolated" && cfg.margin_mode != "crossed") {
        result.add_error(
            std::format("futures.margin_mode '{}' неизвестен; "
                        "допустимые: isolated, crossed",
                        cfg.margin_mode));
    }

    // margin_coin
    if (cfg.margin_coin.empty()) {
        result.add_error("futures.margin_coin не может быть пустым");
    }

    // Leverage: [1, 125]
    if (cfg.default_leverage < 1 || cfg.default_leverage > 125) {
        result.add_error(
            std::format("futures.default_leverage {} вне допустимого диапазона [1, 125]",
                        cfg.default_leverage));
    }
    if (cfg.max_leverage < 1 || cfg.max_leverage > 125) {
        result.add_error(
            std::format("futures.max_leverage {} вне допустимого диапазона [1, 125]",
                        cfg.max_leverage));
    }
    if (cfg.default_leverage > cfg.max_leverage) {
        result.add_error(
            std::format("futures.default_leverage ({}) не может превышать max_leverage ({})",
                        cfg.default_leverage, cfg.max_leverage));
    }

    // Leverage по режимам: [1, max_leverage]
    auto check_regime_leverage = [&](int lev, const char* name) {
        if (lev < 1 || lev > cfg.max_leverage) {
            result.add_error(
                std::format("futures.{} ({}) вне допустимого диапазона [1, {}]",
                            name, lev, cfg.max_leverage));
        }
    };
    check_regime_leverage(cfg.leverage_trending, "leverage_trending");
    check_regime_leverage(cfg.leverage_ranging, "leverage_ranging");
    check_regime_leverage(cfg.leverage_volatile, "leverage_volatile");
    check_regime_leverage(cfg.leverage_stress, "leverage_stress");

    // Liquidation buffer: (0, 50]
    if (cfg.liquidation_buffer_pct <= 0.0 || cfg.liquidation_buffer_pct > 50.0) {
        result.add_error(
            std::format("futures.liquidation_buffer_pct {} вне допустимого диапазона (0, 50]",
                        cfg.liquidation_buffer_pct));
    }

    // max_leverage_drawdown_scale: [0, 1]
    if (cfg.max_leverage_drawdown_scale < 0.0 || cfg.max_leverage_drawdown_scale > 1.0) {
        result.add_error(
            std::format("futures.max_leverage_drawdown_scale {} вне допустимого диапазона [0, 1]",
                        cfg.max_leverage_drawdown_scale));
    }

    // Funding rate threshold: [0, 1] (в относительных единицах)
    if (cfg.funding_rate_threshold < 0.0 || cfg.funding_rate_threshold > 1.0) {
        result.add_error(
            std::format("futures.funding_rate_threshold {} вне допустимого диапазона [0, 1]",
                        cfg.funding_rate_threshold));
    }

    // Funding rate penalty: [0, 1]
    if (cfg.funding_rate_penalty < 0.0 || cfg.funding_rate_penalty > 1.0) {
        result.add_error(
            std::format("futures.funding_rate_penalty {} вне допустимого диапазона [0, 1]",
                        cfg.funding_rate_penalty));
    }

    // Maintenance margin rate: (0, 0.1] (Bitget max 10%)
    if (cfg.maintenance_margin_rate <= 0.0 || cfg.maintenance_margin_rate > 0.1) {
        result.add_error(
            std::format("futures.maintenance_margin_rate {} вне допустимого диапазона (0, 0.1]",
                        cfg.maintenance_margin_rate));
    }
}

} // namespace tb::config
