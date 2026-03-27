/**
 * @file config_loader.cpp
 * @brief Реализация YAML загрузчика конфигурации
 * 
 * Базовый парсер YAML с поддержкой вложенных секций.
 * Не поддерживает сложные YAML структуры (списки, якоря и т.д.).
 */
#include "config_loader.hpp"
#include "config_hash.hpp"
#include "config_validator.hpp"
#include "common/enums.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <stdexcept>

namespace tb::config {

// ============================================================
// Вспомогательные функции парсинга
// ============================================================

namespace {

/// Удаляет пробелы с начала и конца строки
std::string trim(std::string_view s) {
    auto start = s.begin();
    auto end   = s.end();
    while (start != end && std::isspace(static_cast<unsigned char>(*start))) ++start;
    while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(start, end);
}

/// Удаляет кавычки вокруг строки (если есть)
std::string unquote(std::string s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

/// Подсчёт отступа (количество пробелов в начале строки)
int indent_level(std::string_view line) {
    int count = 0;
    for (char c : line) {
        if (c == ' ') ++count;
        else break;
    }
    return count;
}

} // anonymous namespace

// ============================================================
// YamlConfigLoader
// ============================================================

std::pair<std::string, std::string>
YamlConfigLoader::parse_kv_line(std::string_view line) {
    auto colon_pos = line.find(':');
    if (colon_pos == std::string_view::npos) {
        return {"", ""};
    }
    std::string key   = trim(line.substr(0, colon_pos));
    std::string value = trim(line.substr(colon_pos + 1));
    // Удаляем комментарии
    auto comment = value.find('#');
    if (comment != std::string::npos) {
        value = trim(value.substr(0, comment));
    }
    return {key, unquote(value)};
}

std::string
YamlConfigLoader::get_value(
    const std::unordered_map<std::string, std::string>& kv,
    const std::string& key,
    const std::string& default_val)
{
    auto it = kv.find(key);
    return (it != kv.end()) ? it->second : default_val;
}

std::unordered_map<std::string, std::string>
YamlConfigLoader::parse_yaml_flat(std::string_view content) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream stream{std::string(content)};
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        // Пропускаем комментарии и пустые строки
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int indent = indent_level(line);
        auto [key, value] = parse_kv_line(trimmed);

        if (key.empty()) continue;

        if (value.empty() && indent == 0) {
            // Это заголовок секции
            current_section = key;
        } else {
            // Это значение
            std::string full_key = current_section.empty()
                ? key
                : current_section + "." + key;
            result[full_key] = value;
        }
    }

    return result;
}

Result<AppConfig> YamlConfigLoader::load(std::string_view path) {
    // Читаем файл
    std::ifstream file{std::string(path)};
    if (!file.is_open()) {
        return Err<AppConfig>(TbError::ConfigLoadFailed);
    }

    std::ostringstream buf;
    buf << file.rdbuf();
    std::string content = buf.str();

    if (content.empty()) {
        return Err<AppConfig>(TbError::ConfigLoadFailed);
    }

    // Вычисляем хеш для аудита
    std::string hash;
    try {
        hash = compute_config_hash(content);
    } catch (...) {
        hash = "hash_computation_failed";
    }

    // Парсим YAML в плоскую карту
    auto kv = parse_yaml_flat(content);

    // Заполняем AppConfig из карты
    AppConfig cfg;
    cfg.config_hash = hash;

    // Секция trading
    auto mode_str = get_value(kv, "trading.mode", "paper");
    auto mode = trading_mode_from_string(mode_str);
    cfg.trading.mode = mode.value_or(TradingMode::Paper);
    cfg.trading.initial_capital = std::stod(
        get_value(kv, "trading.initial_capital", "10000.0"));

    // Секция exchange
    cfg.exchange.endpoint_rest  = get_value(kv, "exchange.endpoint_rest",  "https://api.bitget.com");
    cfg.exchange.endpoint_ws    = get_value(kv, "exchange.endpoint_ws",    "wss://ws.bitget.com/v2/ws/public");
    cfg.exchange.api_key_ref    = get_value(kv, "exchange.api_key_ref",    "BITGET_API_KEY");
    cfg.exchange.api_secret_ref = get_value(kv, "exchange.api_secret_ref", "BITGET_API_SECRET");
    cfg.exchange.passphrase_ref = get_value(kv, "exchange.passphrase_ref", "BITGET_PASSPHRASE");
    auto timeout_str = get_value(kv, "exchange.timeout_ms", "5000");
    try {
        cfg.exchange.timeout_ms = std::stoi(timeout_str);
    } catch (...) {
        cfg.exchange.timeout_ms = 5000;
    }

    // Секция logging
    cfg.logging.level            = get_value(kv, "logging.level",           "info");
    cfg.logging.output_path      = get_value(kv, "logging.output_path",     "-");
    auto json_str = get_value(kv, "logging.structured_json", "false");
    cfg.logging.structured_json  = (json_str == "true" || json_str == "1");

    // Секция metrics
    auto metrics_enabled_str = get_value(kv, "metrics.enabled", "true");
    cfg.metrics.enabled = (metrics_enabled_str != "false" && metrics_enabled_str != "0");
    auto metrics_port_str = get_value(kv, "metrics.port", "9090");
    try {
        cfg.metrics.port = std::stoi(metrics_port_str);
    } catch (...) {
        cfg.metrics.port = 9090;
    }
    cfg.metrics.path = get_value(kv, "metrics.path", "/metrics");

    // Секция health
    auto health_enabled_str = get_value(kv, "health.enabled", "true");
    cfg.health.enabled = (health_enabled_str != "false" && health_enabled_str != "0");
    auto health_port_str = get_value(kv, "health.port", "8080");
    try {
        cfg.health.port = std::stoi(health_port_str);
    } catch (...) {
        cfg.health.port = 8080;
    }

    // Секция risk
    auto parse_double = [](const std::string& s, double def) -> double {
        try { return std::stod(s); } catch (...) { return def; }
    };
    cfg.risk.max_position_notional = parse_double(get_value(kv, "risk.max_position_notional", "10000"), 10000.0);
    cfg.risk.max_daily_loss_pct    = parse_double(get_value(kv, "risk.max_daily_loss_pct",    "2.0"),   2.0);
    cfg.risk.max_drawdown_pct      = parse_double(get_value(kv, "risk.max_drawdown_pct",      "5.0"),   5.0);
    auto ks_str = get_value(kv, "risk.kill_switch_enabled", "true");
    cfg.risk.kill_switch_enabled   = (ks_str != "false" && ks_str != "0");

    // Секция pair_selection
    auto ps_mode_str = get_value(kv, "pair_selection.mode", "auto");
    cfg.pair_selection.mode = (ps_mode_str == "manual")
        ? PairSelectionMode::Manual : PairSelectionMode::Auto;
    auto top_n_str = get_value(kv, "pair_selection.top_n", "5");
    try { cfg.pair_selection.top_n = std::stoi(top_n_str); }
    catch (...) { cfg.pair_selection.top_n = 5; }
    cfg.pair_selection.min_volume_usdt = parse_double(
        get_value(kv, "pair_selection.min_volume_usdt", "500000"), 500000.0);
    cfg.pair_selection.max_spread_bps = parse_double(
        get_value(kv, "pair_selection.max_spread_bps", "50"), 50.0);
    auto rotation_str = get_value(kv, "pair_selection.rotation_interval_hours", "24");
    try { cfg.pair_selection.rotation_interval_hours = std::stoi(rotation_str); }
    catch (...) { cfg.pair_selection.rotation_interval_hours = 24; }

    // Парсинг comma-separated списков (manual_symbols, blacklist)
    auto parse_list = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> result;
        if (s.empty()) return result;
        std::istringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto trimmed_item = item;
            trimmed_item.erase(0, trimmed_item.find_first_not_of(" \t"));
            trimmed_item.erase(trimmed_item.find_last_not_of(" \t") + 1);
            if (!trimmed_item.empty()) result.push_back(trimmed_item);
        }
        return result;
    };
    cfg.pair_selection.manual_symbols = parse_list(
        get_value(kv, "pair_selection.symbols", ""));
    cfg.pair_selection.blacklist = parse_list(
        get_value(kv, "pair_selection.blacklist", ""));

    // Секция adversarial_defense
    auto adv_enabled_str = get_value(kv, "adversarial_defense.enabled", "true");
    cfg.adversarial_defense.enabled = (adv_enabled_str != "false" && adv_enabled_str != "0");
    auto adv_fail_closed_str = get_value(kv, "adversarial_defense.fail_closed_on_invalid_data", "true");
    cfg.adversarial_defense.fail_closed_on_invalid_data =
        (adv_fail_closed_str != "false" && adv_fail_closed_str != "0");
    auto adv_auto_cd_str = get_value(kv, "adversarial_defense.auto_cooldown_on_veto", "true");
    cfg.adversarial_defense.auto_cooldown_on_veto =
        (adv_auto_cd_str != "false" && adv_auto_cd_str != "0");
    cfg.adversarial_defense.auto_cooldown_severity = parse_double(
        get_value(kv, "adversarial_defense.auto_cooldown_severity", "0.85"), 0.85);
    cfg.adversarial_defense.spread_explosion_threshold_bps = parse_double(
        get_value(kv, "adversarial_defense.spread_explosion_threshold_bps", "100.0"), 100.0);
    cfg.adversarial_defense.spread_normal_bps = parse_double(
        get_value(kv, "adversarial_defense.spread_normal_bps", "20.0"), 20.0);
    cfg.adversarial_defense.min_liquidity_depth = parse_double(
        get_value(kv, "adversarial_defense.min_liquidity_depth", "50.0"), 50.0);
    cfg.adversarial_defense.book_imbalance_threshold = parse_double(
        get_value(kv, "adversarial_defense.book_imbalance_threshold", "0.8"), 0.8);
    cfg.adversarial_defense.book_instability_threshold = parse_double(
        get_value(kv, "adversarial_defense.book_instability_threshold", "0.7"), 0.7);
    cfg.adversarial_defense.toxic_flow_ratio_threshold = parse_double(
        get_value(kv, "adversarial_defense.toxic_flow_ratio_threshold", "1.8"), 1.8);
    cfg.adversarial_defense.aggressive_flow_threshold = parse_double(
        get_value(kv, "adversarial_defense.aggressive_flow_threshold", "0.8"), 0.8);
    cfg.adversarial_defense.vpin_toxic_threshold = parse_double(
        get_value(kv, "adversarial_defense.vpin_toxic_threshold", "0.7"), 0.7);
    cfg.adversarial_defense.cooldown_duration_ms = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.cooldown_duration_ms", "30000"), 30000.0));
    cfg.adversarial_defense.post_shock_cooldown_ms = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.post_shock_cooldown_ms", "60000"), 60000.0));
    cfg.adversarial_defense.max_market_data_age_ns = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.max_market_data_age_ns", "2000000000"), 2'000'000'000.0));
    cfg.adversarial_defense.max_confidence_reduction = parse_double(
        get_value(kv, "adversarial_defense.max_confidence_reduction", "0.8"), 0.8);
    cfg.adversarial_defense.max_threshold_expansion = parse_double(
        get_value(kv, "adversarial_defense.max_threshold_expansion", "2.0"), 2.0);
    cfg.adversarial_defense.compound_threat_factor = parse_double(
        get_value(kv, "adversarial_defense.compound_threat_factor", "0.5"), 0.5);
    cfg.adversarial_defense.cooldown_severity_scale = parse_double(
        get_value(kv, "adversarial_defense.cooldown_severity_scale", "1.5"), 1.5);
    cfg.adversarial_defense.recovery_duration_ms = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.recovery_duration_ms", "10000"), 10000.0));
    cfg.adversarial_defense.recovery_confidence_floor = parse_double(
        get_value(kv, "adversarial_defense.recovery_confidence_floor", "0.6"), 0.6);
    cfg.adversarial_defense.spread_velocity_threshold_bps_per_sec = parse_double(
        get_value(kv, "adversarial_defense.spread_velocity_threshold_bps_per_sec", "50.0"), 50.0);
    // --- Adaptive baseline ---
    cfg.adversarial_defense.baseline_alpha = parse_double(
        get_value(kv, "adversarial_defense.baseline_alpha", "0.01"), 0.01);
    cfg.adversarial_defense.baseline_warmup_ticks = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.baseline_warmup_ticks", "200"), 200.0));
    cfg.adversarial_defense.z_score_spread_threshold = parse_double(
        get_value(kv, "adversarial_defense.z_score_spread_threshold", "3.0"), 3.0);
    cfg.adversarial_defense.z_score_depth_threshold = parse_double(
        get_value(kv, "adversarial_defense.z_score_depth_threshold", "3.0"), 3.0);
    cfg.adversarial_defense.z_score_ratio_threshold = parse_double(
        get_value(kv, "adversarial_defense.z_score_ratio_threshold", "3.0"), 3.0);
    cfg.adversarial_defense.baseline_stale_reset_ms = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.baseline_stale_reset_ms", "300000"), 300000.0));
    // --- Threat memory ---
    cfg.adversarial_defense.threat_memory_alpha = parse_double(
        get_value(kv, "adversarial_defense.threat_memory_alpha", "0.15"), 0.15);
    cfg.adversarial_defense.threat_memory_residual_factor = parse_double(
        get_value(kv, "adversarial_defense.threat_memory_residual_factor", "0.3"), 0.3);
    cfg.adversarial_defense.threat_escalation_ticks = static_cast<int>(parse_double(
        get_value(kv, "adversarial_defense.threat_escalation_ticks", "5"), 5.0));
    cfg.adversarial_defense.threat_escalation_boost = parse_double(
        get_value(kv, "adversarial_defense.threat_escalation_boost", "0.1"), 0.1);
    // --- Depth asymmetry ---
    cfg.adversarial_defense.depth_asymmetry_threshold = parse_double(
        get_value(kv, "adversarial_defense.depth_asymmetry_threshold", "0.3"), 0.3);
    // --- Cross-signal amplification ---
    cfg.adversarial_defense.cross_signal_amplification = parse_double(
        get_value(kv, "adversarial_defense.cross_signal_amplification", "0.3"), 0.3);
    // --- v4: Percentile scoring ---
    cfg.adversarial_defense.percentile_window_size = static_cast<int>(parse_double(
        get_value(kv, "adversarial_defense.percentile_window_size", "500"), 500.0));
    cfg.adversarial_defense.percentile_severity_threshold = parse_double(
        get_value(kv, "adversarial_defense.percentile_severity_threshold", "0.95"), 0.95);
    // --- v4: Correlation matrix ---
    cfg.adversarial_defense.correlation_alpha = parse_double(
        get_value(kv, "adversarial_defense.correlation_alpha", "0.02"), 0.02);
    cfg.adversarial_defense.correlation_breakdown_threshold = parse_double(
        get_value(kv, "adversarial_defense.correlation_breakdown_threshold", "0.4"), 0.4);
    // --- v4: Multi-timeframe ---
    cfg.adversarial_defense.baseline_halflife_fast_ms = parse_double(
        get_value(kv, "adversarial_defense.baseline_halflife_fast_ms", "30000"), 30000.0);
    cfg.adversarial_defense.baseline_halflife_medium_ms = parse_double(
        get_value(kv, "adversarial_defense.baseline_halflife_medium_ms", "300000"), 300000.0);
    cfg.adversarial_defense.baseline_halflife_slow_ms = parse_double(
        get_value(kv, "adversarial_defense.baseline_halflife_slow_ms", "1800000"), 1800000.0);
    cfg.adversarial_defense.timeframe_divergence_threshold = parse_double(
        get_value(kv, "adversarial_defense.timeframe_divergence_threshold", "2.5"), 2.5);
    // --- v4: Hysteresis ---
    cfg.adversarial_defense.hysteresis_enter_severity = parse_double(
        get_value(kv, "adversarial_defense.hysteresis_enter_severity", "0.5"), 0.5);
    cfg.adversarial_defense.hysteresis_exit_severity = parse_double(
        get_value(kv, "adversarial_defense.hysteresis_exit_severity", "0.25"), 0.25);
    cfg.adversarial_defense.hysteresis_confidence_penalty = parse_double(
        get_value(kv, "adversarial_defense.hysteresis_confidence_penalty", "0.15"), 0.15);
    // --- v4: Event sourcing ---
    cfg.adversarial_defense.audit_log_max_size = static_cast<int64_t>(parse_double(
        get_value(kv, "adversarial_defense.audit_log_max_size", "10000"), 10000.0));

    // Секция ai_advisory
    auto ai_enabled_str = get_value(kv, "ai_advisory.enabled", "false");
    cfg.ai_advisory.enabled = (ai_enabled_str == "true" || ai_enabled_str == "1");
    cfg.ai_advisory.timeout_ms = static_cast<int>(parse_double(
        get_value(kv, "ai_advisory.timeout_ms", "2000"), 2000.0));
    cfg.ai_advisory.max_confidence_adjustment = parse_double(
        get_value(kv, "ai_advisory.max_confidence_adjustment", "0.5"), 0.5);
    cfg.ai_advisory.veto_severity_threshold = parse_double(
        get_value(kv, "ai_advisory.veto_severity_threshold", "0.8"), 0.8);
    cfg.ai_advisory.cooldown_ms = static_cast<int64_t>(parse_double(
        get_value(kv, "ai_advisory.cooldown_ms", "5000"), 5000.0));
    // Detector thresholds
    cfg.ai_advisory.thresholds.volatility_ratio_threshold = parse_double(
        get_value(kv, "ai_advisory.volatility_ratio_threshold", "3.0"), 3.0);
    cfg.ai_advisory.thresholds.volatility_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.volatility_confidence_adj", "-0.3"), -0.3);
    cfg.ai_advisory.thresholds.rsi_overbought = parse_double(
        get_value(kv, "ai_advisory.rsi_overbought", "85.0"), 85.0);
    cfg.ai_advisory.thresholds.rsi_oversold = parse_double(
        get_value(kv, "ai_advisory.rsi_oversold", "15.0"), 15.0);
    cfg.ai_advisory.thresholds.rsi_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.rsi_confidence_adj", "-0.15"), -0.15);
    cfg.ai_advisory.thresholds.spread_anomaly_bps = parse_double(
        get_value(kv, "ai_advisory.spread_anomaly_bps", "50.0"), 50.0);
    cfg.ai_advisory.thresholds.spread_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.spread_confidence_adj", "-0.25"), -0.25);
    cfg.ai_advisory.thresholds.liquidity_ratio_min = parse_double(
        get_value(kv, "ai_advisory.liquidity_ratio_min", "0.3"), 0.3);
    cfg.ai_advisory.thresholds.liquidity_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.liquidity_confidence_adj", "-0.2"), -0.2);
    cfg.ai_advisory.thresholds.vpin_toxic_threshold = parse_double(
        get_value(kv, "ai_advisory.vpin_toxic_threshold", "0.7"), 0.7);
    cfg.ai_advisory.thresholds.vpin_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.vpin_confidence_adj", "-0.25"), -0.25);
    cfg.ai_advisory.thresholds.book_imbalance_threshold = parse_double(
        get_value(kv, "ai_advisory.book_imbalance_threshold", "0.7"), 0.7);
    cfg.ai_advisory.thresholds.book_imbalance_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.book_imbalance_confidence_adj", "-0.15"), -0.15);
    cfg.ai_advisory.thresholds.book_instability_threshold = parse_double(
        get_value(kv, "ai_advisory.book_instability_threshold", "0.7"), 0.7);
    cfg.ai_advisory.thresholds.book_instability_confidence_adj = parse_double(
        get_value(kv, "ai_advisory.book_instability_confidence_adj", "-0.15"), -0.15);
    // Hysteresis, ensemble escalation, caution mode
    cfg.ai_advisory.caution_severity_threshold = parse_double(
        get_value(kv, "ai_advisory.caution_severity_threshold", "0.55"), 0.55);
    cfg.ai_advisory.hysteresis_clear_ticks = static_cast<int>(parse_double(
        get_value(kv, "ai_advisory.hysteresis_clear_ticks", "3"), 3.0));
    cfg.ai_advisory.ensemble_escalation_count = static_cast<int>(parse_double(
        get_value(kv, "ai_advisory.ensemble_escalation_count", "3"), 3.0));
    cfg.ai_advisory.ensemble_escalation_bonus = parse_double(
        get_value(kv, "ai_advisory.ensemble_escalation_bonus", "0.15"), 0.15);
    cfg.ai_advisory.caution_size_multiplier = parse_double(
        get_value(kv, "ai_advisory.caution_size_multiplier", "0.5"), 0.5);
    cfg.ai_advisory.use_severity_weighted_adjustment = (
        get_value(kv, "ai_advisory.use_severity_weighted_adjustment", "true") != "false");

    // ============================================================
    // Decision Engine
    // ============================================================
    cfg.decision.min_conviction_threshold = parse_double(
        get_value(kv, "decision.min_conviction_threshold", "0.28"), 0.28);
    cfg.decision.conflict_dominance_threshold = parse_double(
        get_value(kv, "decision.conflict_dominance_threshold", "0.60"), 0.60);

    // Advanced decision features
    cfg.decision.enable_regime_threshold_scaling = (
        get_value(kv, "decision.enable_regime_threshold_scaling", "true") != "false");
    cfg.decision.enable_regime_dominance_scaling = (
        get_value(kv, "decision.enable_regime_dominance_scaling", "true") != "false");
    cfg.decision.enable_time_decay = (
        get_value(kv, "decision.enable_time_decay", "true") != "false");
    cfg.decision.time_decay_halflife_ms = parse_double(
        get_value(kv, "decision.time_decay_halflife_ms", "700"), 700.0);
    cfg.decision.enable_ensemble_conviction = (
        get_value(kv, "decision.enable_ensemble_conviction", "true") != "false");
    cfg.decision.ensemble_agreement_bonus = parse_double(
        get_value(kv, "decision.ensemble_agreement_bonus", "0.08"), 0.08);
    cfg.decision.ensemble_max_bonus = parse_double(
        get_value(kv, "decision.ensemble_max_bonus", "0.20"), 0.20);
    cfg.decision.enable_portfolio_awareness = (
        get_value(kv, "decision.enable_portfolio_awareness", "true") != "false");
    cfg.decision.drawdown_boost_scale = parse_double(
        get_value(kv, "decision.drawdown_boost_scale", "0.10"), 0.10);
    cfg.decision.enable_execution_cost_modeling = (
        get_value(kv, "decision.enable_execution_cost_modeling", "true") != "false");
    cfg.decision.max_acceptable_cost_bps = parse_double(
        get_value(kv, "decision.max_acceptable_cost_bps", "80"), 80.0);
    cfg.decision.enable_time_skew_detection = (
        get_value(kv, "decision.enable_time_skew_detection", "true") != "false");

    // ============================================================
    // Trading Params (position management)
    // ============================================================
    cfg.trading_params.atr_stop_multiplier = parse_double(
        get_value(kv, "trading_params.atr_stop_multiplier", "2.0"), 2.0);
    cfg.trading_params.max_loss_per_trade_pct = parse_double(
        get_value(kv, "trading_params.max_loss_per_trade_pct", "1.0"), 1.0);
    cfg.trading_params.breakeven_atr_threshold = parse_double(
        get_value(kv, "trading_params.breakeven_atr_threshold", "1.5"), 1.5);
    cfg.trading_params.partial_tp_atr_threshold = parse_double(
        get_value(kv, "trading_params.partial_tp_atr_threshold", "2.0"), 2.0);
    cfg.trading_params.partial_tp_fraction = parse_double(
        get_value(kv, "trading_params.partial_tp_fraction", "0.5"), 0.5);
    cfg.trading_params.max_hold_loss_minutes = static_cast<int>(parse_double(
        get_value(kv, "trading_params.max_hold_loss_minutes", "15"), 15));
    cfg.trading_params.max_hold_absolute_minutes = static_cast<int>(parse_double(
        get_value(kv, "trading_params.max_hold_absolute_minutes", "60"), 60));
    cfg.trading_params.order_cooldown_seconds = static_cast<int>(parse_double(
        get_value(kv, "trading_params.order_cooldown_seconds", "10"), 10));
    cfg.trading_params.stop_loss_cooldown_seconds = static_cast<int>(parse_double(
        get_value(kv, "trading_params.stop_loss_cooldown_seconds", "300"), 300));

    // ── Исполнительная альфа (execution_alpha) ────────────────────────────
    cfg.execution_alpha.max_spread_bps_passive = parse_double(
        get_value(kv, "execution_alpha.max_spread_bps_passive", "15.0"), 15.0);
    cfg.execution_alpha.max_spread_bps_any = parse_double(
        get_value(kv, "execution_alpha.max_spread_bps_any", "50.0"), 50.0);
    cfg.execution_alpha.adverse_selection_threshold = parse_double(
        get_value(kv, "execution_alpha.adverse_selection_threshold", "0.7"), 0.7);
    cfg.execution_alpha.urgency_passive_threshold = parse_double(
        get_value(kv, "execution_alpha.urgency_passive_threshold", "0.5"), 0.5);
    cfg.execution_alpha.urgency_aggressive_threshold = parse_double(
        get_value(kv, "execution_alpha.urgency_aggressive_threshold", "0.8"), 0.8);
    cfg.execution_alpha.large_order_slice_threshold = parse_double(
        get_value(kv, "execution_alpha.large_order_slice_threshold", "0.1"), 0.1);
    cfg.execution_alpha.vpin_toxic_threshold = parse_double(
        get_value(kv, "execution_alpha.vpin_toxic_threshold", "0.65"), 0.65);
    cfg.execution_alpha.vpin_weight = parse_double(
        get_value(kv, "execution_alpha.vpin_weight", "0.40"), 0.40);
    cfg.execution_alpha.imbalance_favorable_threshold = parse_double(
        get_value(kv, "execution_alpha.imbalance_favorable_threshold", "0.30"), 0.30);
    cfg.execution_alpha.imbalance_unfavorable_threshold = parse_double(
        get_value(kv, "execution_alpha.imbalance_unfavorable_threshold", "0.30"), 0.30);
    cfg.execution_alpha.use_weighted_mid_price =
        (get_value(kv, "execution_alpha.use_weighted_mid_price", "true") != "false");
    cfg.execution_alpha.limit_price_passive_bps = parse_double(
        get_value(kv, "execution_alpha.limit_price_passive_bps", "3.0"), 3.0);
    cfg.execution_alpha.urgency_cusum_boost = parse_double(
        get_value(kv, "execution_alpha.urgency_cusum_boost", "0.15"), 0.15);
    cfg.execution_alpha.urgency_tod_weight = parse_double(
        get_value(kv, "execution_alpha.urgency_tod_weight", "0.10"), 0.10);
    cfg.execution_alpha.min_fill_probability_passive = parse_double(
        get_value(kv, "execution_alpha.min_fill_probability_passive", "0.25"), 0.25);
    cfg.execution_alpha.postonly_spread_threshold_bps = parse_double(
        get_value(kv, "execution_alpha.postonly_spread_threshold_bps", "4.5"), 4.5);
    cfg.execution_alpha.postonly_urgency_max = parse_double(
        get_value(kv, "execution_alpha.postonly_urgency_max", "0.35"), 0.35);
    cfg.execution_alpha.postonly_adverse_max = parse_double(
        get_value(kv, "execution_alpha.postonly_adverse_max", "0.35"), 0.35);

    // ── Opportunity Cost ──────────────────────────────────────────────────
    cfg.opportunity_cost.min_net_expected_bps = parse_double(
        get_value(kv, "opportunity_cost.min_net_expected_bps", "0.0"), 0.0);
    cfg.opportunity_cost.execute_min_net_bps = parse_double(
        get_value(kv, "opportunity_cost.execute_min_net_bps", "15.0"), 15.0);
    cfg.opportunity_cost.high_exposure_threshold = parse_double(
        get_value(kv, "opportunity_cost.high_exposure_threshold", "0.75"), 0.75);
    cfg.opportunity_cost.high_exposure_min_conviction = parse_double(
        get_value(kv, "opportunity_cost.high_exposure_min_conviction", "0.65"), 0.65);
    cfg.opportunity_cost.max_symbol_concentration = parse_double(
        get_value(kv, "opportunity_cost.max_symbol_concentration", "0.25"), 0.25);
    cfg.opportunity_cost.max_strategy_concentration = parse_double(
        get_value(kv, "opportunity_cost.max_strategy_concentration", "0.35"), 0.35);
    cfg.opportunity_cost.capital_exhaustion_threshold = parse_double(
        get_value(kv, "opportunity_cost.capital_exhaustion_threshold", "0.90"), 0.90);
    cfg.opportunity_cost.weight_conviction = parse_double(
        get_value(kv, "opportunity_cost.weight_conviction", "0.35"), 0.35);
    cfg.opportunity_cost.weight_net_edge = parse_double(
        get_value(kv, "opportunity_cost.weight_net_edge", "0.35"), 0.35);
    cfg.opportunity_cost.weight_capital_efficiency = parse_double(
        get_value(kv, "opportunity_cost.weight_capital_efficiency", "0.15"), 0.15);
    cfg.opportunity_cost.weight_urgency = parse_double(
        get_value(kv, "opportunity_cost.weight_urgency", "0.15"), 0.15);
    cfg.opportunity_cost.conviction_to_bps_scale = parse_double(
        get_value(kv, "opportunity_cost.conviction_to_bps_scale", "120.0"), 120.0);
    cfg.opportunity_cost.upgrade_min_edge_advantage_bps = parse_double(
        get_value(kv, "opportunity_cost.upgrade_min_edge_advantage_bps", "10.0"), 10.0);
    cfg.opportunity_cost.drawdown_penalty_scale = parse_double(
        get_value(kv, "opportunity_cost.drawdown_penalty_scale", "0.5"), 0.5);

    // ── Секция regime ──
    // Trend thresholds
    cfg.regime.trend.adx_strong = parse_double(
        get_value(kv, "regime.trend_adx_strong", "30.0"), 30.0);
    cfg.regime.trend.adx_weak_min = parse_double(
        get_value(kv, "regime.trend_adx_weak_min", "18.0"), 18.0);
    cfg.regime.trend.adx_weak_max = parse_double(
        get_value(kv, "regime.trend_adx_weak_max", "30.0"), 30.0);
    cfg.regime.trend.rsi_trend_bias = parse_double(
        get_value(kv, "regime.trend_rsi_bias", "50.0"), 50.0);

    // Mean-reversion thresholds
    cfg.regime.mean_reversion.rsi_overbought = parse_double(
        get_value(kv, "regime.mr_rsi_overbought", "70.0"), 70.0);
    cfg.regime.mean_reversion.rsi_oversold = parse_double(
        get_value(kv, "regime.mr_rsi_oversold", "30.0"), 30.0);
    cfg.regime.mean_reversion.adx_max = parse_double(
        get_value(kv, "regime.mr_adx_max", "25.0"), 25.0);

    // Volatility thresholds
    cfg.regime.volatility.bb_bandwidth_expansion = parse_double(
        get_value(kv, "regime.vol_bb_expansion", "0.06"), 0.06);
    cfg.regime.volatility.bb_bandwidth_compression = parse_double(
        get_value(kv, "regime.vol_bb_compression", "0.02"), 0.02);
    cfg.regime.volatility.atr_norm_expansion = parse_double(
        get_value(kv, "regime.vol_atr_expansion", "0.02"), 0.02);
    cfg.regime.volatility.adx_compression_max = parse_double(
        get_value(kv, "regime.vol_adx_compression_max", "20.0"), 20.0);

    // Stress thresholds
    cfg.regime.stress.rsi_extreme_high = parse_double(
        get_value(kv, "regime.stress_rsi_extreme_high", "85.0"), 85.0);
    cfg.regime.stress.rsi_extreme_low = parse_double(
        get_value(kv, "regime.stress_rsi_extreme_low", "15.0"), 15.0);
    cfg.regime.stress.obv_norm_extreme = parse_double(
        get_value(kv, "regime.stress_obv_extreme", "2.0"), 2.0);
    cfg.regime.stress.aggressive_flow_toxic = parse_double(
        get_value(kv, "regime.stress_aggressive_flow", "0.75"), 0.75);
    cfg.regime.stress.spread_toxic_bps = parse_double(
        get_value(kv, "regime.stress_spread_toxic_bps", "15.0"), 15.0);
    cfg.regime.stress.book_instability_threshold = parse_double(
        get_value(kv, "regime.stress_book_instability", "0.6"), 0.6);
    cfg.regime.stress.spread_stress_bps = parse_double(
        get_value(kv, "regime.stress_spread_bps", "30.0"), 30.0);
    cfg.regime.stress.liquidity_ratio_stress = parse_double(
        get_value(kv, "regime.stress_liquidity_ratio", "3.0"), 3.0);

    // Chop thresholds
    cfg.regime.chop.adx_max = parse_double(
        get_value(kv, "regime.chop_adx_max", "18.0"), 18.0);

    // Transition / hysteresis policy
    cfg.regime.transition.confirmation_ticks = static_cast<int>(parse_double(
        get_value(kv, "regime.transition_confirmation_ticks", "3"), 3.0));
    cfg.regime.transition.min_confidence_to_switch = parse_double(
        get_value(kv, "regime.transition_min_confidence", "0.55"), 0.55);
    cfg.regime.transition.inertia_alpha = parse_double(
        get_value(kv, "regime.transition_inertia_alpha", "0.15"), 0.15);
    cfg.regime.transition.dwell_time_ticks = static_cast<int>(parse_double(
        get_value(kv, "regime.transition_dwell_ticks", "5"), 5.0));

    // Confidence policy
    cfg.regime.confidence.base_confidence = parse_double(
        get_value(kv, "regime.confidence_base", "0.5"), 0.5);
    cfg.regime.confidence.data_quality_weight = parse_double(
        get_value(kv, "regime.confidence_data_quality_weight", "0.2"), 0.2);
    cfg.regime.confidence.max_indicator_count = static_cast<int>(parse_double(
        get_value(kv, "regime.confidence_max_indicators", "6"), 6.0));
    cfg.regime.confidence.anomaly_confidence = parse_double(
        get_value(kv, "regime.confidence_anomaly", "0.9"), 0.9);
    cfg.regime.confidence.same_regime_stability = parse_double(
        get_value(kv, "regime.stability_same_regime", "0.9"), 0.9);
    cfg.regime.confidence.first_classification_stability = parse_double(
        get_value(kv, "regime.stability_first_classification", "0.5"), 0.5);

    // Валидация
    ConfigValidator validator;
    auto validation = validator.validate(cfg);
    if (!validation) {
        return Err<AppConfig>(TbError::ConfigValidationFailed);
    }

    return Ok(std::move(cfg));
}

std::unique_ptr<IConfigLoader> create_config_loader() {
    return std::make_unique<YamlConfigLoader>();
}

} // namespace tb::config
