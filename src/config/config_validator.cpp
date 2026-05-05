/**
 * @file config_validator.cpp
 * @brief Реализация валидатора конфигурации
 */
#include "config_validator.hpp"
#include <format>

namespace tb::config {

VoidResult ConfigValidator::validate(const AppConfig& cfg) const {
    auto result = validate_detailed(cfg);
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
    validate_opportunity_cost(cfg.opportunity_cost, result);
    validate_pair_selection(cfg.pair_selection, result);
    validate_trading_params(cfg.trading_params, result);
    validate_decision(cfg.decision, result);
    validate_execution_alpha(cfg.execution_alpha, result);
    validate_world_model(cfg.world_model, result);
    validate_regime(cfg.regime, result);
    validate_futures(cfg.futures, result);
    validate_uncertainty(cfg.uncertainty, result);
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
    if (cfg.consecutive_loss_penalty < 0.0) {
        result.add_error("opportunity_cost.consecutive_loss_penalty должен быть >= 0");
    }
}

void ConfigValidator::validate_pair_selection(
    const PairSelectionConfig& cfg, ValidationResult& result) const {
    if (cfg.top_n < 1 || cfg.top_n > 50) {
        result.add_error(
            std::format("pair_selection.top_n {} вне допустимого диапазона [1, 50]", cfg.top_n));
    }
    if (cfg.min_volume_usdt <= 0.0) {
        result.add_error("pair_selection.min_volume_usdt должен быть > 0");
    }
    if (cfg.max_spread_bps <= 0.0 || cfg.max_spread_bps > 500.0) {
        result.add_error("pair_selection.max_spread_bps должен быть в диапазоне (0, 500]");
    }
    if (cfg.rotation_interval_hours < 1 || cfg.rotation_interval_hours > 168) {
        result.add_error("pair_selection.rotation_interval_hours должен быть в диапазоне [1, 168]");
    }
    if (cfg.max_candidates_for_candles < 1) {
        result.add_error("pair_selection.max_candidates_for_candles должен быть >= 1");
    }
    if (cfg.scan_timeout_ms < 1000 || cfg.scan_timeout_ms > 300'000) {
        result.add_error("pair_selection.scan_timeout_ms должен быть в диапазоне [1000, 300000]");
    }
    if (cfg.max_correlation_in_basket <= 0.0 || cfg.max_correlation_in_basket > 1.0) {
        result.add_error("pair_selection.max_correlation_in_basket должен быть в (0, 1]");
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
    if (cfg.partial_tp_fraction <= 0.0 || cfg.partial_tp_fraction >= 1.0) {
        result.add_error("trading_params.partial_tp_fraction должен быть в диапазоне (0, 1)");
    }
    if (cfg.order_cooldown_seconds < 0 || cfg.order_cooldown_seconds > 3600) {
        result.add_error("trading_params.order_cooldown_seconds должен быть в диапазоне [0, 3600]");
    }
    if (cfg.stop_loss_cooldown_seconds < 0 || cfg.stop_loss_cooldown_seconds > 86400) {
        result.add_error("trading_params.stop_loss_cooldown_seconds должен быть в диапазоне [0, 86400]");
    }
    // Ценовой стоп-лосс (страховочный лимит)
    if (cfg.price_stop_loss_pct <= 0.0 || cfg.price_stop_loss_pct > 50.0) {
        result.add_error("trading_params.price_stop_loss_pct должен быть в диапазоне (0, 50]");
    }
    // Risk:Reward ratio
    if (cfg.min_risk_reward_ratio < 0.0 || cfg.min_risk_reward_ratio > 100.0) {
        result.add_error("trading_params.min_risk_reward_ratio должен быть в диапазоне [0, 100]");
    }
    // Dust threshold
    if (cfg.dust_threshold_usdt < 0.0) {
        result.add_error("trading_params.dust_threshold_usdt должен быть >= 0");
    }
    // Quick profit
    if (cfg.quick_profit_fee_multiplier < 1.0) {
        result.add_error("trading_params.quick_profit_fee_multiplier должен быть >= 1.0");
    }
    // PnL gate
    if (cfg.pnl_gate_loss_pct < 0.0 || cfg.pnl_gate_loss_pct > 100.0) {
        result.add_error("trading_params.pnl_gate_loss_pct должен быть в диапазоне [0, 100]");
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
    if (cfg.taker_fee_bps < 0.0) {
        result.add_error("execution_alpha.taker_fee_bps должен быть >= 0");
    }
    if (cfg.maker_fee_bps < 0.0) {
        result.add_error("execution_alpha.maker_fee_bps должен быть >= 0");
    }
}

void ConfigValidator::validate_world_model(
    const WorldModelConfig& cfg, ValidationResult& result) const {
    if (!cfg.validate()) {
        result.add_error("world_model конфигурация невалидна");
    }
}

void ConfigValidator::validate_cross(const AppConfig& cfg, ValidationResult& result) const {
    // Kill-switch обязателен
    if (!cfg.risk.kill_switch_enabled) {
        result.add_error("risk.kill_switch_enabled обязателен");
    }
    // Бот ТОЛЬКО фьючерсный — futures.enabled обязателен
    if (!cfg.futures.enabled) {
        result.add_error("CRITICAL: futures.enabled MUST be true. Bot is FUTURES-ONLY.");
    }
    // Высокое плечо — предупреждение
    if (cfg.futures.enabled && cfg.futures.max_leverage > 20) {
        result.add_warning(
            std::format("futures.max_leverage ({}) > 20 — высокий риск ликвидации",
                        cfg.futures.max_leverage));
    }
    // Начальный капитал должен покрывать хотя бы один минимальный ордер
    if (cfg.trading.initial_capital <= 0.0) {
        result.add_error("trading.initial_capital должен быть > 0");
    }
    // max_loss_per_trade не должен превышать daily loss limit
    if (cfg.trading_params.max_loss_per_trade_pct > cfg.risk.max_daily_loss_pct) {
        result.add_error("trading_params.max_loss_per_trade_pct не может превышать risk.max_daily_loss_pct");
    }
    // Плечо по режимам: volatile ≤ ranging ≤ trending (естественный порядок)
    // ИСПРАВЛЕНИЕ M2 (аудит): повышено до error — это production-противоречие
    if (cfg.futures.leverage_volatile > cfg.futures.leverage_ranging) {
        result.add_error(
            std::format("futures: leverage_volatile ({}) > leverage_ranging ({}) — "
                        "в волатильном режиме плечо выше, чем в боковике",
                        cfg.futures.leverage_volatile, cfg.futures.leverage_ranging));
    }
    if (cfg.futures.leverage_ranging > cfg.futures.leverage_trending) {
        result.add_error(
            std::format("futures: leverage_ranging ({}) > leverage_trending ({}) — "
                        "в боковике плечо выше, чем в тренде",
                        cfg.futures.leverage_ranging, cfg.futures.leverage_trending));
    }
    // ИСПРАВЛЕНИЕ M2 (аудит): семантический конфликт — leverage floor может обнулить смысл режимного плеча
    if (cfg.futures.min_leverage > cfg.futures.leverage_volatile) {
        result.add_error(
            std::format("futures: min_leverage ({}) > leverage_volatile ({}) — "
                        "floor плеча выше режимного значения volatile, поведение непредсказуемо",
                        cfg.futures.min_leverage, cfg.futures.leverage_volatile));
    }
    // price_stop_loss_pct × max_leverage не должен превышать 100% (полная потеря капитала)
    double max_loss_at_max_lev = cfg.trading_params.price_stop_loss_pct *
                                  static_cast<double>(cfg.futures.max_leverage);
    if (max_loss_at_max_lev > 100.0) {
        result.add_error(
            std::format("price_stop_loss_pct ({:.1f}%) × max_leverage ({}) = {:.1f}% — "
                        "превышает 100%%, возможна полная потеря капитала",
                        cfg.trading_params.price_stop_loss_pct,
                        cfg.futures.max_leverage, max_loss_at_max_lev));
    }

    // === Micro-account / exchange-minimum cross-checks ===
    // Bitget minTradeUSDT varies: ~$5 for BTC, ~$1 for altcoins. Use $5 as safe ceiling.
    constexpr double kMaxExchangeMinNotional = 5.0;
    const double max_leveraged_capital = cfg.trading.initial_capital *
                                          static_cast<double>(cfg.futures.max_leverage);

    // Capital × max_leverage must cover at least one minimum exchange order
    if (max_leveraged_capital < kMaxExchangeMinNotional) {
        result.add_error(
            std::format("initial_capital ({:.2f}) × max_leverage ({}) = {:.2f} USDT — "
                        "недостаточно для минимального ордера на бирже ({:.0f} USDT)",
                        cfg.trading.initial_capital, cfg.futures.max_leverage,
                        max_leveraged_capital, kMaxExchangeMinNotional));
    }

    // max_position_notional should not exceed leveraged capital
    // ИСПРАВЛЕНИЕ M2 (аудит): повышено до error — недостижимые лимиты вводят оператора в заблуждение
    if (cfg.risk.max_position_notional > max_leveraged_capital && max_leveraged_capital > 0.0) {
        result.add_error(
            std::format("risk.max_position_notional ({:.0f}) > capital×leverage ({:.2f}) — "
                        "лимит позиции недостижим при текущем капитале и плече",
                        cfg.risk.max_position_notional, max_leveraged_capital));
    }

    // Warn about micro-account operation
    if (cfg.trading.initial_capital < 50.0) {
        result.add_warning(
            std::format("Микро-аккаунт: initial_capital = {:.2f} USDT — "
                        "бот работает вблизи биржевых минимумов, высокая чувствительность к комиссиям",
                        cfg.trading.initial_capital));
    }
}

void ConfigValidator::validate_regime(
    const regime::RegimeConfig& cfg, ValidationResult& result) const {

    // === Trend thresholds ===
    if (cfg.trend.adx_strong <= 0.0 || cfg.trend.adx_strong > 100.0) {
        result.add_error("regime.trend.adx_strong должен быть в диапазоне (0, 100]");
    }
    if (cfg.trend.adx_weak_min <= 0.0 || cfg.trend.adx_weak_min > cfg.trend.adx_strong) {
        result.add_error("regime.trend.adx_weak_min должен быть в (0, adx_strong]");
    }
    if (cfg.trend.adx_weak_max < cfg.trend.adx_weak_min || cfg.trend.adx_weak_max > 100.0) {
        result.add_error("regime.trend.adx_weak_max должен быть в [adx_weak_min, 100]");
    }
    if (cfg.trend.rsi_trend_bias <= 50.0 || cfg.trend.rsi_trend_bias >= 70.0) {
        result.add_error("regime.trend.rsi_trend_bias должен быть в (50, 70) для корректного directional bias");
    }

    // === Mean-reversion thresholds ===
    if (cfg.mean_reversion.rsi_overbought <= 50.0 || cfg.mean_reversion.rsi_overbought > 95.0) {
        result.add_error("regime.mean_reversion.rsi_overbought должен быть в (50, 95]");
    }
    if (cfg.mean_reversion.rsi_oversold < 5.0 || cfg.mean_reversion.rsi_oversold >= 50.0) {
        result.add_error("regime.mean_reversion.rsi_oversold должен быть в [5, 50)");
    }
    if (cfg.mean_reversion.adx_max <= 0.0 || cfg.mean_reversion.adx_max > 50.0) {
        result.add_error("regime.mean_reversion.adx_max должен быть в (0, 50]");
    }

    // === Volatility thresholds ===
    if (cfg.volatility.bb_bandwidth_compression >= cfg.volatility.bb_bandwidth_expansion) {
        result.add_error("regime.volatility.bb_bandwidth_compression должен быть < bb_bandwidth_expansion");
    }
    if (cfg.volatility.bb_bandwidth_expansion <= 0.0 || cfg.volatility.bb_bandwidth_expansion > 1.0) {
        result.add_error("regime.volatility.bb_bandwidth_expansion должен быть в (0, 1]");
    }
    if (cfg.volatility.bb_bandwidth_compression <= 0.0) {
        result.add_error("regime.volatility.bb_bandwidth_compression должен быть > 0");
    }
    if (cfg.volatility.atr_norm_expansion <= 0.0 || cfg.volatility.atr_norm_expansion > 0.5) {
        result.add_error("regime.volatility.atr_norm_expansion должен быть в (0, 0.5]");
    }
    if (cfg.volatility.adx_compression_max <= 0.0 || cfg.volatility.adx_compression_max > 50.0) {
        result.add_error("regime.volatility.adx_compression_max должен быть в (0, 50]");
    }

    // === Stress thresholds ===
    if (cfg.stress.rsi_extreme_high <= cfg.mean_reversion.rsi_overbought) {
        result.add_error("regime.stress.rsi_extreme_high должен быть > mean_reversion.rsi_overbought");
    }
    if (cfg.stress.rsi_extreme_low >= cfg.mean_reversion.rsi_oversold) {
        result.add_error("regime.stress.rsi_extreme_low должен быть < mean_reversion.rsi_oversold");
    }
    if (cfg.stress.obv_norm_extreme <= 0.0 || cfg.stress.obv_norm_extreme > 10.0) {
        result.add_error("regime.stress.obv_norm_extreme должен быть в (0, 10]");
    }
    if (cfg.stress.aggressive_flow_toxic <= 0.0 || cfg.stress.aggressive_flow_toxic >= 1.0) {
        result.add_error("regime.stress.aggressive_flow_toxic должен быть в (0, 1)");
    }
    if (cfg.stress.spread_toxic_bps <= 0.0 || cfg.stress.spread_toxic_bps > 200.0) {
        result.add_error("regime.stress.spread_toxic_bps должен быть в (0, 200]");
    }
    if (cfg.stress.spread_stress_bps <= cfg.stress.spread_toxic_bps) {
        result.add_error("regime.stress.spread_stress_bps должен быть > spread_toxic_bps");
    }
    if (cfg.stress.book_instability_threshold <= 0.0 || cfg.stress.book_instability_threshold >= 1.0) {
        result.add_error("regime.stress.book_instability_threshold должен быть в (0, 1)");
    }
    // liquidity_ratio ∈ [0,1], порог стресса: низкий ratio = перекос книги
    if (cfg.stress.liquidity_ratio_stress <= 0.0 || cfg.stress.liquidity_ratio_stress >= 1.0) {
        result.add_error("regime.stress.liquidity_ratio_stress должен быть в (0, 1)");
    }

    // === Chop thresholds ===
    if (cfg.chop.adx_max <= 0.0 || cfg.chop.adx_max > cfg.trend.adx_weak_min) {
        result.add_error("regime.chop.adx_max должен быть в (0, trend.adx_weak_min]");
    }

    // === Transition policy ===
    if (cfg.transition.confirmation_ticks < 1 || cfg.transition.confirmation_ticks > 20) {
        result.add_error("regime.transition.confirmation_ticks должен быть в [1, 20]");
    }
    if (cfg.transition.min_confidence_to_switch <= 0.0 || cfg.transition.min_confidence_to_switch >= 1.0) {
        result.add_error("regime.transition.min_confidence_to_switch должен быть в (0, 1)");
    }
    if (cfg.transition.dwell_time_ticks < 1 || cfg.transition.dwell_time_ticks > 50) {
        result.add_error("regime.transition.dwell_time_ticks должен быть в [1, 50]");
    }

    // === Confidence policy ===
    if (cfg.confidence.base_confidence <= 0.0 || cfg.confidence.base_confidence >= 1.0) {
        result.add_error("regime.confidence.base_confidence должен быть в (0, 1)");
    }
    if (cfg.confidence.data_quality_weight < 0.0 || cfg.confidence.data_quality_weight > 1.0) {
        result.add_error("regime.confidence.data_quality_weight должен быть в [0, 1]");
    }
    if (cfg.confidence.max_indicator_count < 1 || cfg.confidence.max_indicator_count > 50) {
        result.add_error("regime.confidence.max_indicator_count должен быть в [1, 50]");
    }
    if (cfg.confidence.anomaly_confidence <= 0.0 || cfg.confidence.anomaly_confidence > 1.0) {
        result.add_error("regime.confidence.anomaly_confidence должен быть в (0, 1]");
    }
    if (cfg.confidence.same_regime_stability <= 0.0 || cfg.confidence.same_regime_stability > 1.0) {
        result.add_error("regime.confidence.same_regime_stability должен быть в (0, 1]");
    }
    if (cfg.confidence.first_classification_stability < 0.0 || cfg.confidence.first_classification_stability > 1.0) {
        result.add_error("regime.confidence.first_classification_stability должен быть в [0, 1]");
    }
}

void ConfigValidator::validate_futures(const FuturesConfig& cfg, ValidationResult& result) const {

    // product_type
    if (cfg.product_type != "USDT-FUTURES") {
        result.add_error(
            std::format("futures.product_type '{}' недопустим; "
                        "бот поддерживает только USDT-FUTURES",
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
    if (cfg.min_leverage < 1 || cfg.min_leverage > cfg.max_leverage) {
        result.add_error(
            std::format("futures.min_leverage ({}) вне допустимого диапазона [1, {}]",
                        cfg.min_leverage, cfg.max_leverage));
    }
    if (cfg.min_leverage > cfg.default_leverage) {
        result.add_error(
            std::format("futures.min_leverage ({}) не может превышать default_leverage ({})",
                        cfg.min_leverage, cfg.default_leverage));
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

    // Liquidation buffer: (0, 50]
    if (cfg.liquidation_buffer_pct <= 0.0 || cfg.liquidation_buffer_pct > 50.0) {
        result.add_error(
            std::format("futures.liquidation_buffer_pct {} вне допустимого диапазона (0, 50]",
                        cfg.liquidation_buffer_pct));
    }

    // Funding rate threshold: (0, 0.1] (decimal: 0.0005 = 0.05% per 8h)
    if (cfg.funding_rate_threshold <= 0.0 || cfg.funding_rate_threshold > 0.1) {
        result.add_error(
            std::format("futures.funding_rate_threshold {} вне допустимого диапазона (0, 0.1]",
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

    // === LeverageEngineConfig validation ===
    const auto& le = cfg.leverage_engine;

    // Volatility breakpoints: must be strictly ordered
    if (le.vol_low_atr <= 0.0 || le.vol_mid_atr <= le.vol_low_atr ||
        le.vol_high_atr <= le.vol_mid_atr || le.vol_extreme_atr <= le.vol_high_atr) {
        result.add_error(
            std::format("futures.leverage_engine.vol_*: breakpoints не упорядочены: "
                        "{} < {} < {} < {} требуется",
                        le.vol_low_atr, le.vol_mid_atr, le.vol_high_atr, le.vol_extreme_atr));
    }

    // Vol floor: (0, 1]
    if (le.vol_floor <= 0.0 || le.vol_floor > 1.0) {
        result.add_error(
            std::format("futures.leverage_engine.vol_floor {} вне (0, 1]", le.vol_floor));
    }

    // Conviction: min_mult ∈ [0, 1], breakpoint ∈ (0, 1), max_mult ∈ [1, 3]
    if (le.conviction_min_mult < 0.0 || le.conviction_min_mult > 1.0) {
        result.add_error(
            std::format("futures.leverage_engine.conviction_min_mult {} вне [0, 1]",
                        le.conviction_min_mult));
    }
    if (le.conviction_breakpoint <= 0.0 || le.conviction_breakpoint >= 1.0) {
        result.add_error(
            std::format("futures.leverage_engine.conviction_breakpoint {} вне (0, 1)",
                        le.conviction_breakpoint));
    }
    if (le.conviction_max_mult < 1.0 || le.conviction_max_mult > 3.0) {
        result.add_error(
            std::format("futures.leverage_engine.conviction_max_mult {} вне [1, 3]",
                        le.conviction_max_mult));
    }

    // Drawdown: floor ∈ [0, 1], halfpoint > 0
    if (le.drawdown_floor_mult < 0.0 || le.drawdown_floor_mult > 1.0) {
        result.add_error(
            std::format("futures.leverage_engine.drawdown_floor_mult {} вне [0, 1]",
                        le.drawdown_floor_mult));
    }
    if (le.drawdown_halfpoint_pct <= 0.0) {
        result.add_error(
            std::format("futures.leverage_engine.drawdown_halfpoint_pct {} должен быть > 0",
                        le.drawdown_halfpoint_pct));
    }

    // EMA alpha: (0, 1]
    if (le.ema_alpha <= 0.0 || le.ema_alpha > 1.0) {
        result.add_error(
            std::format("futures.leverage_engine.ema_alpha {} вне (0, 1]", le.ema_alpha));
    }

    // Taker fee rate: [0, 0.01] (max 1%)
    if (le.taker_fee_rate < 0.0 || le.taker_fee_rate > 0.01) {
        result.add_error(
            std::format("futures.leverage_engine.taker_fee_rate {} вне [0, 0.01]",
                        le.taker_fee_rate));
    }
}

void ConfigValidator::validate_uncertainty(
    const UncertaintyConfig& cfg, ValidationResult& result) const {

    // Weights must be positive
    auto check_weight = [&](double w, const char* name) {
        if (w < 0.0 || w > 1.0) {
            result.add_error(std::format("uncertainty.{} {} вне диапазона [0, 1]", name, w));
        }
    };
    check_weight(cfg.w_regime, "w_regime");
    check_weight(cfg.w_signal, "w_signal");
    check_weight(cfg.w_data_quality, "w_data_quality");
    check_weight(cfg.w_execution, "w_execution");
    check_weight(cfg.w_portfolio, "w_portfolio");
    check_weight(cfg.w_ml, "w_ml");
    check_weight(cfg.w_correlation, "w_correlation");
    check_weight(cfg.w_transition, "w_transition");
    check_weight(cfg.w_operational, "w_operational");

    // Weights should sum to ~1.0 (tolerance 0.01)
    double total = cfg.w_regime + cfg.w_signal + cfg.w_data_quality + cfg.w_execution +
                   cfg.w_portfolio + cfg.w_ml + cfg.w_correlation + cfg.w_transition +
                   cfg.w_operational;
    if (std::abs(total - 1.0) > 0.01) {
        result.add_warning(std::format(
            "uncertainty: сумма весов = {} (ожидается ~1.0)", total));
    }

    // Thresholds must be ordered: 0 < low < moderate < high <= 1
    if (cfg.threshold_low <= 0.0 || cfg.threshold_low >= cfg.threshold_moderate) {
        result.add_error("uncertainty: threshold_low должен быть в (0, threshold_moderate)");
    }
    if (cfg.threshold_moderate >= cfg.threshold_high) {
        result.add_error("uncertainty: threshold_moderate должен быть < threshold_high");
    }
    if (cfg.threshold_high > 1.0) {
        result.add_error("uncertainty: threshold_high должен быть <= 1.0");
    }

    // Hysteresis
    if (cfg.hysteresis_up < 0.0 || cfg.hysteresis_up > 0.2) {
        result.add_error(std::format("uncertainty.hysteresis_up {} вне [0, 0.2]", cfg.hysteresis_up));
    }
    if (cfg.hysteresis_down < 0.0 || cfg.hysteresis_down > 0.2) {
        result.add_error(std::format("uncertainty.hysteresis_down {} вне [0, 0.2]", cfg.hysteresis_down));
    }

    // EMA alpha
    if (cfg.ema_alpha <= 0.0 || cfg.ema_alpha >= 1.0) {
        result.add_error(std::format("uncertainty.ema_alpha {} вне (0, 1)", cfg.ema_alpha));
    }

    // Size floor
    if (cfg.size_floor < 0.0 || cfg.size_floor > 0.5) {
        result.add_error(std::format("uncertainty.size_floor {} вне [0, 0.5]", cfg.size_floor));
    }

    // Threshold ceiling
    if (cfg.threshold_ceiling < 1.0 || cfg.threshold_ceiling > 5.0) {
        result.add_error(std::format("uncertainty.threshold_ceiling {} вне [1, 5]", cfg.threshold_ceiling));
    }

    // Cooldown
    if (cfg.consecutive_extreme_for_cooldown < 1 || cfg.consecutive_extreme_for_cooldown > 20) {
        result.add_error(std::format("uncertainty.consecutive_extreme_for_cooldown {} вне [1, 20]",
                                     cfg.consecutive_extreme_for_cooldown));
    }
    if (cfg.cooldown_duration_ns < 1'000'000'000LL || cfg.cooldown_duration_ns > 600'000'000'000LL) {
        result.add_error("uncertainty.cooldown_duration_ns вне [1s, 600s]");
    }
    if (cfg.consecutive_high_for_defensive < 1 || cfg.consecutive_high_for_defensive > 20) {
        result.add_error(std::format("uncertainty.consecutive_high_for_defensive {} вне [1, 20]",
                                     cfg.consecutive_high_for_defensive));
    }

    // Liquidity ratio threshold
    if (cfg.liquidity_ratio_penalty_threshold <= 0.0 || cfg.liquidity_ratio_penalty_threshold > 1.0) {
        result.add_error(std::format("uncertainty.liquidity_ratio_penalty_threshold {} вне (0, 1]",
                                     cfg.liquidity_ratio_penalty_threshold));
    }
}

} // namespace tb::config
