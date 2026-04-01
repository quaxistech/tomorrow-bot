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
#include <cstdio>
#include <iostream>

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
    // Удаляем комментарии (пропускаем # внутри кавычек)
    bool in_single_quote = false;
    bool in_double_quote = false;
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\'' && !in_double_quote) in_single_quote = !in_single_quote;
        else if (c == '"' && !in_single_quote) in_double_quote = !in_double_quote;
        else if (c == '#' && !in_single_quote && !in_double_quote) {
            value = trim(value.substr(0, i));
            break;
        }
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
        std::fprintf(stderr, "[config_loader] WARN: не удалось вычислить хеш конфигурации\n");
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
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать exchange.timeout_ms (value='%s'), default=5000\n", timeout_str.c_str());
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
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать metrics.port (value='%s'), default=9090\n", metrics_port_str.c_str());
        cfg.metrics.port = 9090;
    }
    cfg.metrics.path = get_value(kv, "metrics.path", "/metrics");

    // Секция risk
    auto parse_double = [](const std::string& s, double def, const char* field_name = "") -> double {
        try { return std::stod(s); }
        catch (...) {
            if (field_name[0] != '\0') {
                std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать '%s' (value='%s'), используется default=%.6g\n",
                              field_name, s.c_str(), def);
            }
            return def;
        }
    };
    cfg.risk.max_position_notional = parse_double(get_value(kv, "risk.max_position_notional", "10000"), 10000.0, "risk.max_position_notional");
    cfg.risk.max_daily_loss_pct    = parse_double(get_value(kv, "risk.max_daily_loss_pct",    "2.0"),   2.0, "risk.max_daily_loss_pct");
    cfg.risk.max_drawdown_pct      = parse_double(get_value(kv, "risk.max_drawdown_pct",      "5.0"),   5.0, "risk.max_drawdown_pct");
    auto ks_str = get_value(kv, "risk.kill_switch_enabled", "true");
    cfg.risk.kill_switch_enabled   = (ks_str != "false" && ks_str != "0");

    // Расширенные параметры риска (desk-grade)
    cfg.risk.max_strategy_daily_loss_pct   = parse_double(get_value(kv, "risk.max_strategy_daily_loss_pct",   "1.5"),  1.5, "risk.max_strategy_daily_loss_pct");
    cfg.risk.max_strategy_exposure_pct     = parse_double(get_value(kv, "risk.max_strategy_exposure_pct",     "30.0"), 30.0, "risk.max_strategy_exposure_pct");
    cfg.risk.max_symbol_concentration_pct  = parse_double(get_value(kv, "risk.max_symbol_concentration_pct",  "35.0"), 35.0, "risk.max_symbol_concentration_pct");
    auto sdp_str = get_value(kv, "risk.max_same_direction_positions", "3");
    try { cfg.risk.max_same_direction_positions = std::stoi(sdp_str); }
    catch (...) {
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать risk.max_same_direction_positions (value='%s'), default=3\n", sdp_str.c_str());
        cfg.risk.max_same_direction_positions = 3;
    }
    auto ral_str = get_value(kv, "risk.regime_aware_limits_enabled", "true");
    cfg.risk.regime_aware_limits_enabled   = (ral_str != "false" && ral_str != "0");
    cfg.risk.stress_regime_scale           = parse_double(get_value(kv, "risk.stress_regime_scale",           "0.5"),  0.5, "risk.stress_regime_scale");
    cfg.risk.trending_regime_scale         = parse_double(get_value(kv, "risk.trending_regime_scale",         "1.2"),  1.2, "risk.trending_regime_scale");
    cfg.risk.chop_regime_scale             = parse_double(get_value(kv, "risk.chop_regime_scale",             "0.7"),  0.7, "risk.chop_regime_scale");
    auto tph_str = get_value(kv, "risk.max_trades_per_hour", "8");
    try { cfg.risk.max_trades_per_hour = std::stoi(tph_str); }
    catch (...) {
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать risk.max_trades_per_hour (value='%s'), default=8\n", tph_str.c_str());
        cfg.risk.max_trades_per_hour = 8;
    }
    cfg.risk.min_trade_interval_sec        = parse_double(get_value(kv, "risk.min_trade_interval_sec",        "30.0"), 30.0, "risk.min_trade_interval_sec");
    cfg.risk.max_adverse_excursion_pct     = parse_double(get_value(kv, "risk.max_adverse_excursion_pct",     "3.0"),  3.0, "risk.max_adverse_excursion_pct");
    cfg.risk.max_realized_daily_loss_pct   = parse_double(get_value(kv, "risk.max_realized_daily_loss_pct",   "1.5"),  1.5, "risk.max_realized_daily_loss_pct");
    auto cutoff_str = get_value(kv, "risk.utc_cutoff_hour", "-1");
    try { cfg.risk.utc_cutoff_hour = std::stoi(cutoff_str); }
    catch (...) {
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать risk.utc_cutoff_hour (value='%s'), default=-1\n", cutoff_str.c_str());
        cfg.risk.utc_cutoff_hour = -1;
    }

    // Секция pair_selection
    auto ps_mode_str = get_value(kv, "pair_selection.mode", "auto");
    cfg.pair_selection.mode = (ps_mode_str == "manual")
        ? PairSelectionMode::Manual : PairSelectionMode::Auto;
    auto top_n_str = get_value(kv, "pair_selection.top_n", "5");
    try { cfg.pair_selection.top_n = std::stoi(top_n_str); }
    catch (...) {
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать pair_selection.top_n (value='%s'), default=5\n", top_n_str.c_str());
        cfg.pair_selection.top_n = 5;
    }
    cfg.pair_selection.min_volume_usdt = parse_double(
        get_value(kv, "pair_selection.min_volume_usdt", "500000"), 500000.0, "pair_selection.min_volume_usdt");
    cfg.pair_selection.max_spread_bps = parse_double(
        get_value(kv, "pair_selection.max_spread_bps", "50"), 50.0, "pair_selection.max_spread_bps");
    auto rotation_str = get_value(kv, "pair_selection.rotation_interval_hours", "24");
    try { cfg.pair_selection.rotation_interval_hours = std::stoi(rotation_str); }
    catch (...) {
        std::fprintf(stderr, "[config_loader] WARN: не удалось разобрать pair_selection.rotation_interval_hours (value='%s'), default=24\n", rotation_str.c_str());
        cfg.pair_selection.rotation_interval_hours = 24;
    }

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

    // Расширенные настройки pair_selection
    auto max_cand_str = get_value(kv, "pair_selection.max_candidates_for_candles", "30");
    try { cfg.pair_selection.max_candidates_for_candles = std::stoi(max_cand_str); }
    catch (...) { cfg.pair_selection.max_candidates_for_candles = 30; }
    auto candle_conc_str = get_value(kv, "pair_selection.candle_fetch_concurrency", "5");
    try { cfg.pair_selection.candle_fetch_concurrency = std::stoi(candle_conc_str); }
    catch (...) { cfg.pair_selection.candle_fetch_concurrency = 5; }
    auto candle_hist_str = get_value(kv, "pair_selection.candle_history_hours", "48");
    try { cfg.pair_selection.candle_history_hours = std::stoi(candle_hist_str); }
    catch (...) { cfg.pair_selection.candle_history_hours = 48; }
    auto scan_to_str = get_value(kv, "pair_selection.scan_timeout_ms", "60000");
    try { cfg.pair_selection.scan_timeout_ms = std::stoi(scan_to_str); }
    catch (...) { cfg.pair_selection.scan_timeout_ms = 60000; }
    auto api_retry_str = get_value(kv, "pair_selection.api_retry_max", "3");
    try { cfg.pair_selection.api_retry_max = std::stoi(api_retry_str); }
    catch (...) { cfg.pair_selection.api_retry_max = 3; }
    auto api_retry_delay_str = get_value(kv, "pair_selection.api_retry_base_delay_ms", "200");
    try { cfg.pair_selection.api_retry_base_delay_ms = std::stoi(api_retry_delay_str); }
    catch (...) { cfg.pair_selection.api_retry_base_delay_ms = 200; }
    auto cb_thr_str = get_value(kv, "pair_selection.circuit_breaker_threshold", "5");
    try { cfg.pair_selection.circuit_breaker_threshold = std::stoi(cb_thr_str); }
    catch (...) { cfg.pair_selection.circuit_breaker_threshold = 5; }
    auto cb_reset_str = get_value(kv, "pair_selection.circuit_breaker_reset_ms", "300000");
    try { cfg.pair_selection.circuit_breaker_reset_ms = std::stoi(cb_reset_str); }
    catch (...) { cfg.pair_selection.circuit_breaker_reset_ms = 300000; }
    cfg.pair_selection.max_correlation_in_basket = parse_double(
        get_value(kv, "pair_selection.max_correlation_in_basket", "0.85"), 0.85, "pair_selection.max_correlation_in_basket");
    auto mpp_str = get_value(kv, "pair_selection.max_pairs_per_sector", "2");
    try { cfg.pair_selection.max_pairs_per_sector = std::stoi(mpp_str); }
    catch (...) { cfg.pair_selection.max_pairs_per_sector = 2; }
    cfg.pair_selection.min_liquidity_depth_usdt = parse_double(
        get_value(kv, "pair_selection.min_liquidity_depth_usdt", "50000"), 50000.0, "pair_selection.min_liquidity_depth_usdt");
    auto div_str = get_value(kv, "pair_selection.enable_diversification", "true");
    cfg.pair_selection.enable_diversification = (div_str != "false" && div_str != "0");
    auto persist_scan_str = get_value(kv, "pair_selection.persist_scan_results", "true");
    cfg.pair_selection.persist_scan_results = (persist_scan_str != "false" && persist_scan_str != "0");

    // ScorerConfig (вложенный в pair_selection)
    auto& sc = cfg.pair_selection.scorer;
    sc.version = get_value(kv, "pair_selection.scorer_version", "v4");
    sc.momentum_max = parse_double(get_value(kv, "pair_selection.scorer_momentum_max", "40.0"), 40.0, "pair_selection.scorer_momentum_max");
    sc.trend_max = parse_double(get_value(kv, "pair_selection.scorer_trend_max", "25.0"), 25.0, "pair_selection.scorer_trend_max");
    sc.tradability_max = parse_double(get_value(kv, "pair_selection.scorer_tradability_max", "25.0"), 25.0, "pair_selection.scorer_tradability_max");
    sc.quality_max = parse_double(get_value(kv, "pair_selection.scorer_quality_max", "10.0"), 10.0, "pair_selection.scorer_quality_max");
    sc.momentum_log_multiplier = parse_double(get_value(kv, "pair_selection.scorer_momentum_log_multiplier", "14.5"), 14.5, "pair_selection.scorer_momentum_log_multiplier");
    sc.acceleration_log_multiplier = parse_double(get_value(kv, "pair_selection.scorer_acceleration_log_multiplier", "14.0"), 14.0, "pair_selection.scorer_acceleration_log_multiplier");
    sc.fresh_start_multiplier = parse_double(get_value(kv, "pair_selection.scorer_fresh_start_multiplier", "2.5"), 2.5, "pair_selection.scorer_fresh_start_multiplier");
    sc.fresh_start_roc_24h_cap = parse_double(get_value(kv, "pair_selection.scorer_fresh_start_roc_24h_cap", "10.0"), 10.0, "pair_selection.scorer_fresh_start_roc_24h_cap");
    sc.fresh_start_roc_4h_min = parse_double(get_value(kv, "pair_selection.scorer_fresh_start_roc_4h_min", "0.5"), 0.5, "pair_selection.scorer_fresh_start_roc_4h_min");
    sc.adx_weak_threshold = parse_double(get_value(kv, "pair_selection.scorer_adx_weak_threshold", "15.0"), 15.0, "pair_selection.scorer_adx_weak_threshold");
    sc.adx_moderate_threshold = parse_double(get_value(kv, "pair_selection.scorer_adx_moderate_threshold", "25.0"), 25.0, "pair_selection.scorer_adx_moderate_threshold");
    sc.adx_strong_threshold = parse_double(get_value(kv, "pair_selection.scorer_adx_strong_threshold", "50.0"), 50.0, "pair_selection.scorer_adx_strong_threshold");
    sc.bullish_ratio_min = parse_double(get_value(kv, "pair_selection.scorer_bullish_ratio_min", "0.50"), 0.50, "pair_selection.scorer_bullish_ratio_min");
    sc.roc_normalization_factor = parse_double(get_value(kv, "pair_selection.scorer_roc_normalization_factor", "5.0"), 5.0, "pair_selection.scorer_roc_normalization_factor");
    sc.volume_tier_excellent = parse_double(get_value(kv, "pair_selection.scorer_volume_tier_excellent", "5000000"), 5'000'000.0, "pair_selection.scorer_volume_tier_excellent");
    sc.volume_tier_good = parse_double(get_value(kv, "pair_selection.scorer_volume_tier_good", "1000000"), 1'000'000.0, "pair_selection.scorer_volume_tier_good");
    sc.volume_tier_acceptable = parse_double(get_value(kv, "pair_selection.scorer_volume_tier_acceptable", "500000"), 500'000.0, "pair_selection.scorer_volume_tier_acceptable");
    sc.volume_tier_minimal = parse_double(get_value(kv, "pair_selection.scorer_volume_tier_minimal", "100000"), 100'000.0, "pair_selection.scorer_volume_tier_minimal");
    sc.spread_decay_constant = parse_double(get_value(kv, "pair_selection.scorer_spread_decay_constant", "15.0"), 15.0, "pair_selection.scorer_spread_decay_constant");
    sc.volatility_low_threshold = parse_double(get_value(kv, "pair_selection.scorer_volatility_low_threshold", "0.5"), 0.5, "pair_selection.scorer_volatility_low_threshold");
    sc.volatility_high_threshold = parse_double(get_value(kv, "pair_selection.scorer_volatility_high_threshold", "20.0"), 20.0, "pair_selection.scorer_volatility_high_threshold");
    sc.filter_min_change_24h = parse_double(get_value(kv, "pair_selection.scorer_filter_min_change_24h", "-1.0"), -1.0, "pair_selection.scorer_filter_min_change_24h");
    sc.filter_max_change_24h = parse_double(get_value(kv, "pair_selection.scorer_filter_max_change_24h", "20.0"), 20.0, "pair_selection.scorer_filter_max_change_24h");
    sc.filter_exhausted_pump_24h = parse_double(get_value(kv, "pair_selection.scorer_filter_exhausted_pump_24h", "10.0"), 10.0, "pair_selection.scorer_filter_exhausted_pump_24h");
    sc.filter_exhausted_pump_ratio = parse_double(get_value(kv, "pair_selection.scorer_filter_exhausted_pump_ratio", "0.25"), 0.25, "pair_selection.scorer_filter_exhausted_pump_ratio");
    sc.stagnation_threshold = parse_double(get_value(kv, "pair_selection.scorer_stagnation_threshold", "1.0"), 1.0, "pair_selection.scorer_stagnation_threshold");
    sc.stagnation_penalty = parse_double(get_value(kv, "pair_selection.scorer_stagnation_penalty", "0.3"), 0.3, "pair_selection.scorer_stagnation_penalty");
    sc.steady_gainer_min = parse_double(get_value(kv, "pair_selection.scorer_steady_gainer_min", "2.0"), 2.0, "pair_selection.scorer_steady_gainer_min");
    sc.steady_gainer_max = parse_double(get_value(kv, "pair_selection.scorer_steady_gainer_max", "10.0"), 10.0, "pair_selection.scorer_steady_gainer_max");
    sc.steady_gainer_bonus = parse_double(get_value(kv, "pair_selection.scorer_steady_gainer_bonus", "8.0"), 8.0, "pair_selection.scorer_steady_gainer_bonus");
    sc.negative_change_penalty = parse_double(get_value(kv, "pair_selection.scorer_negative_change_penalty", "0.5"), 0.5, "pair_selection.scorer_negative_change_penalty");
    auto scorer_futures_str = get_value(kv, "pair_selection.scorer_futures_mode", "false");
    sc.futures_mode = (scorer_futures_str != "false" && scorer_futures_str != "0");

    // ============================================================
    // Decision Engine
    // ============================================================
    cfg.decision.min_conviction_threshold = parse_double(
        get_value(kv, "decision.min_conviction_threshold", "0.28"), 0.28, "decision.min_conviction_threshold");
    cfg.decision.conflict_dominance_threshold = parse_double(
        get_value(kv, "decision.conflict_dominance_threshold", "0.60"), 0.60, "decision.conflict_dominance_threshold");

    // Advanced decision features
    cfg.decision.enable_regime_threshold_scaling = (
        get_value(kv, "decision.enable_regime_threshold_scaling", "true") != "false");
    cfg.decision.enable_regime_dominance_scaling = (
        get_value(kv, "decision.enable_regime_dominance_scaling", "true") != "false");
    cfg.decision.enable_time_decay = (
        get_value(kv, "decision.enable_time_decay", "true") != "false");
    cfg.decision.time_decay_halflife_ms = parse_double(
        get_value(kv, "decision.time_decay_halflife_ms", "700"), 700.0, "decision.time_decay_halflife_ms");
    cfg.decision.enable_ensemble_conviction = (
        get_value(kv, "decision.enable_ensemble_conviction", "true") != "false");
    cfg.decision.ensemble_agreement_bonus = parse_double(
        get_value(kv, "decision.ensemble_agreement_bonus", "0.08"), 0.08, "decision.ensemble_agreement_bonus");
    cfg.decision.ensemble_max_bonus = parse_double(
        get_value(kv, "decision.ensemble_max_bonus", "0.20"), 0.20, "decision.ensemble_max_bonus");
    cfg.decision.enable_portfolio_awareness = (
        get_value(kv, "decision.enable_portfolio_awareness", "true") != "false");
    cfg.decision.drawdown_boost_scale = parse_double(
        get_value(kv, "decision.drawdown_boost_scale", "0.10"), 0.10, "decision.drawdown_boost_scale");
    cfg.decision.drawdown_max_boost = parse_double(
        get_value(kv, "decision.drawdown_max_boost", "0.25"), 0.25, "decision.drawdown_max_boost");
    cfg.decision.consecutive_loss_boost = parse_double(
        get_value(kv, "decision.consecutive_loss_boost", "0.03"), 0.03, "decision.consecutive_loss_boost");
    cfg.decision.enable_execution_cost_modeling = (
        get_value(kv, "decision.enable_execution_cost_modeling", "true") != "false");
    cfg.decision.max_acceptable_cost_bps = parse_double(
        get_value(kv, "decision.max_acceptable_cost_bps", "80"), 80.0, "decision.max_acceptable_cost_bps");
    cfg.decision.enable_time_skew_detection = (
        get_value(kv, "decision.enable_time_skew_detection", "true") != "false");

    // ============================================================
    // Trading Params (position management)
    // ============================================================
    cfg.trading_params.atr_stop_multiplier = parse_double(
        get_value(kv, "trading_params.atr_stop_multiplier", "2.0"), 2.0, "trading_params.atr_stop_multiplier");
    cfg.trading_params.max_loss_per_trade_pct = parse_double(
        get_value(kv, "trading_params.max_loss_per_trade_pct", "1.0"), 1.0, "trading_params.max_loss_per_trade_pct");
    cfg.trading_params.price_stop_loss_pct = parse_double(
        get_value(kv, "trading_params.price_stop_loss_pct", "3.0"), 3.0, "trading_params.price_stop_loss_pct");
    cfg.trading_params.min_risk_reward_ratio = parse_double(
        get_value(kv, "trading_params.min_risk_reward_ratio", "1.5"), 1.5, "trading_params.min_risk_reward_ratio");
    cfg.trading_params.breakeven_atr_threshold = parse_double(
        get_value(kv, "trading_params.breakeven_atr_threshold", "1.5"), 1.5, "trading_params.breakeven_atr_threshold");
    cfg.trading_params.partial_tp_atr_threshold = parse_double(
        get_value(kv, "trading_params.partial_tp_atr_threshold", "2.0"), 2.0, "trading_params.partial_tp_atr_threshold");
    cfg.trading_params.partial_tp_fraction = parse_double(
        get_value(kv, "trading_params.partial_tp_fraction", "0.5"), 0.5, "trading_params.partial_tp_fraction");
    cfg.trading_params.max_hold_loss_minutes = static_cast<int>(parse_double(
        get_value(kv, "trading_params.max_hold_loss_minutes", "15"), 15, "trading_params.max_hold_loss_minutes"));
    cfg.trading_params.max_hold_absolute_minutes = static_cast<int>(parse_double(
        get_value(kv, "trading_params.max_hold_absolute_minutes", "60"), 60, "trading_params.max_hold_absolute_minutes"));
    cfg.trading_params.min_hold_minutes = static_cast<int>(parse_double(
        get_value(kv, "trading_params.min_hold_minutes", "30"), 30, "trading_params.min_hold_minutes"));
    cfg.trading_params.order_cooldown_seconds = static_cast<int>(parse_double(
        get_value(kv, "trading_params.order_cooldown_seconds", "10"), 10, "trading_params.order_cooldown_seconds"));
    cfg.trading_params.stop_loss_cooldown_seconds = static_cast<int>(parse_double(
        get_value(kv, "trading_params.stop_loss_cooldown_seconds", "300"), 300, "trading_params.stop_loss_cooldown_seconds"));

    // ── Исполнительная альфа (execution_alpha) ────────────────────────────
    cfg.execution_alpha.max_spread_bps_passive = parse_double(
        get_value(kv, "execution_alpha.max_spread_bps_passive", "15.0"), 15.0, "execution_alpha.max_spread_bps_passive");
    cfg.execution_alpha.max_spread_bps_any = parse_double(
        get_value(kv, "execution_alpha.max_spread_bps_any", "50.0"), 50.0, "execution_alpha.max_spread_bps_any");
    cfg.execution_alpha.adverse_selection_threshold = parse_double(
        get_value(kv, "execution_alpha.adverse_selection_threshold", "0.7"), 0.7, "execution_alpha.adverse_selection_threshold");
    cfg.execution_alpha.urgency_passive_threshold = parse_double(
        get_value(kv, "execution_alpha.urgency_passive_threshold", "0.5"), 0.5, "execution_alpha.urgency_passive_threshold");
    cfg.execution_alpha.urgency_aggressive_threshold = parse_double(
        get_value(kv, "execution_alpha.urgency_aggressive_threshold", "0.8"), 0.8, "execution_alpha.urgency_aggressive_threshold");
    cfg.execution_alpha.large_order_slice_threshold = parse_double(
        get_value(kv, "execution_alpha.large_order_slice_threshold", "0.1"), 0.1, "execution_alpha.large_order_slice_threshold");
    cfg.execution_alpha.vpin_toxic_threshold = parse_double(
        get_value(kv, "execution_alpha.vpin_toxic_threshold", "0.65"), 0.65, "execution_alpha.vpin_toxic_threshold");
    cfg.execution_alpha.vpin_weight = parse_double(
        get_value(kv, "execution_alpha.vpin_weight", "0.40"), 0.40, "execution_alpha.vpin_weight");
    cfg.execution_alpha.imbalance_favorable_threshold = parse_double(
        get_value(kv, "execution_alpha.imbalance_favorable_threshold", "0.30"), 0.30, "execution_alpha.imbalance_favorable_threshold");
    cfg.execution_alpha.imbalance_unfavorable_threshold = parse_double(
        get_value(kv, "execution_alpha.imbalance_unfavorable_threshold", "0.30"), 0.30, "execution_alpha.imbalance_unfavorable_threshold");
    cfg.execution_alpha.use_weighted_mid_price =
        (get_value(kv, "execution_alpha.use_weighted_mid_price", "true") != "false");
    cfg.execution_alpha.limit_price_passive_bps = parse_double(
        get_value(kv, "execution_alpha.limit_price_passive_bps", "3.0"), 3.0, "execution_alpha.limit_price_passive_bps");
    cfg.execution_alpha.urgency_cusum_boost = parse_double(
        get_value(kv, "execution_alpha.urgency_cusum_boost", "0.15"), 0.15, "execution_alpha.urgency_cusum_boost");
    cfg.execution_alpha.urgency_tod_weight = parse_double(
        get_value(kv, "execution_alpha.urgency_tod_weight", "0.10"), 0.10, "execution_alpha.urgency_tod_weight");
    cfg.execution_alpha.min_fill_probability_passive = parse_double(
        get_value(kv, "execution_alpha.min_fill_probability_passive", "0.25"), 0.25, "execution_alpha.min_fill_probability_passive");
    cfg.execution_alpha.postonly_spread_threshold_bps = parse_double(
        get_value(kv, "execution_alpha.postonly_spread_threshold_bps", "4.5"), 4.5, "execution_alpha.postonly_spread_threshold_bps");
    cfg.execution_alpha.postonly_urgency_max = parse_double(
        get_value(kv, "execution_alpha.postonly_urgency_max", "0.35"), 0.35, "execution_alpha.postonly_urgency_max");
    cfg.execution_alpha.postonly_adverse_max = parse_double(
        get_value(kv, "execution_alpha.postonly_adverse_max", "0.35"), 0.35, "execution_alpha.postonly_adverse_max");

    // ── Opportunity Cost ──────────────────────────────────────────────────
    cfg.opportunity_cost.min_net_expected_bps = parse_double(
        get_value(kv, "opportunity_cost.min_net_expected_bps", "0.0"), 0.0, "opportunity_cost.min_net_expected_bps");
    cfg.opportunity_cost.execute_min_net_bps = parse_double(
        get_value(kv, "opportunity_cost.execute_min_net_bps", "15.0"), 15.0, "opportunity_cost.execute_min_net_bps");
    cfg.opportunity_cost.high_exposure_threshold = parse_double(
        get_value(kv, "opportunity_cost.high_exposure_threshold", "0.75"), 0.75, "opportunity_cost.high_exposure_threshold");
    cfg.opportunity_cost.high_exposure_min_conviction = parse_double(
        get_value(kv, "opportunity_cost.high_exposure_min_conviction", "0.65"), 0.65, "opportunity_cost.high_exposure_min_conviction");
    cfg.opportunity_cost.max_symbol_concentration = parse_double(
        get_value(kv, "opportunity_cost.max_symbol_concentration", "0.25"), 0.25, "opportunity_cost.max_symbol_concentration");
    cfg.opportunity_cost.max_strategy_concentration = parse_double(
        get_value(kv, "opportunity_cost.max_strategy_concentration", "0.35"), 0.35, "opportunity_cost.max_strategy_concentration");
    cfg.opportunity_cost.capital_exhaustion_threshold = parse_double(
        get_value(kv, "opportunity_cost.capital_exhaustion_threshold", "0.90"), 0.90, "opportunity_cost.capital_exhaustion_threshold");
    cfg.opportunity_cost.weight_conviction = parse_double(
        get_value(kv, "opportunity_cost.weight_conviction", "0.35"), 0.35, "opportunity_cost.weight_conviction");
    cfg.opportunity_cost.weight_net_edge = parse_double(
        get_value(kv, "opportunity_cost.weight_net_edge", "0.35"), 0.35, "opportunity_cost.weight_net_edge");
    cfg.opportunity_cost.weight_capital_efficiency = parse_double(
        get_value(kv, "opportunity_cost.weight_capital_efficiency", "0.15"), 0.15, "opportunity_cost.weight_capital_efficiency");
    cfg.opportunity_cost.weight_urgency = parse_double(
        get_value(kv, "opportunity_cost.weight_urgency", "0.15"), 0.15, "opportunity_cost.weight_urgency");
    cfg.opportunity_cost.conviction_to_bps_scale = parse_double(
        get_value(kv, "opportunity_cost.conviction_to_bps_scale", "120.0"), 120.0, "opportunity_cost.conviction_to_bps_scale");
    cfg.opportunity_cost.upgrade_min_edge_advantage_bps = parse_double(
        get_value(kv, "opportunity_cost.upgrade_min_edge_advantage_bps", "10.0"), 10.0, "opportunity_cost.upgrade_min_edge_advantage_bps");
    cfg.opportunity_cost.drawdown_penalty_scale = parse_double(
        get_value(kv, "opportunity_cost.drawdown_penalty_scale", "0.5"), 0.5, "opportunity_cost.drawdown_penalty_scale");

    // ── Секция regime ──
    // Trend thresholds
    cfg.regime.trend.adx_strong = parse_double(
        get_value(kv, "regime.trend_adx_strong", "30.0"), 30.0, "regime.trend_adx_strong");
    cfg.regime.trend.adx_weak_min = parse_double(
        get_value(kv, "regime.trend_adx_weak_min", "18.0"), 18.0, "regime.trend_adx_weak_min");
    cfg.regime.trend.adx_weak_max = parse_double(
        get_value(kv, "regime.trend_adx_weak_max", "30.0"), 30.0, "regime.trend_adx_weak_max");
    cfg.regime.trend.rsi_trend_bias = parse_double(
        get_value(kv, "regime.trend_rsi_bias", "50.0"), 50.0, "regime.trend_rsi_bias");

    // Mean-reversion thresholds
    cfg.regime.mean_reversion.rsi_overbought = parse_double(
        get_value(kv, "regime.mr_rsi_overbought", "70.0"), 70.0, "regime.mr_rsi_overbought");
    cfg.regime.mean_reversion.rsi_oversold = parse_double(
        get_value(kv, "regime.mr_rsi_oversold", "30.0"), 30.0, "regime.mr_rsi_oversold");
    cfg.regime.mean_reversion.adx_max = parse_double(
        get_value(kv, "regime.mr_adx_max", "25.0"), 25.0, "regime.mr_adx_max");

    // Volatility thresholds
    cfg.regime.volatility.bb_bandwidth_expansion = parse_double(
        get_value(kv, "regime.vol_bb_expansion", "0.06"), 0.06, "regime.vol_bb_expansion");
    cfg.regime.volatility.bb_bandwidth_compression = parse_double(
        get_value(kv, "regime.vol_bb_compression", "0.02"), 0.02, "regime.vol_bb_compression");
    cfg.regime.volatility.atr_norm_expansion = parse_double(
        get_value(kv, "regime.vol_atr_expansion", "0.02"), 0.02, "regime.vol_atr_expansion");
    cfg.regime.volatility.adx_compression_max = parse_double(
        get_value(kv, "regime.vol_adx_compression_max", "20.0"), 20.0, "regime.vol_adx_compression_max");

    // Stress thresholds
    cfg.regime.stress.rsi_extreme_high = parse_double(
        get_value(kv, "regime.stress_rsi_extreme_high", "85.0"), 85.0, "regime.stress_rsi_extreme_high");
    cfg.regime.stress.rsi_extreme_low = parse_double(
        get_value(kv, "regime.stress_rsi_extreme_low", "15.0"), 15.0, "regime.stress_rsi_extreme_low");
    cfg.regime.stress.obv_norm_extreme = parse_double(
        get_value(kv, "regime.stress_obv_extreme", "2.0"), 2.0, "regime.stress_obv_extreme");
    cfg.regime.stress.aggressive_flow_toxic = parse_double(
        get_value(kv, "regime.stress_aggressive_flow", "0.75"), 0.75, "regime.stress_aggressive_flow");
    cfg.regime.stress.spread_toxic_bps = parse_double(
        get_value(kv, "regime.stress_spread_toxic_bps", "15.0"), 15.0, "regime.stress_spread_toxic_bps");
    cfg.regime.stress.book_instability_threshold = parse_double(
        get_value(kv, "regime.stress_book_instability", "0.6"), 0.6, "regime.stress_book_instability");
    cfg.regime.stress.spread_stress_bps = parse_double(
        get_value(kv, "regime.stress_spread_bps", "30.0"), 30.0, "regime.stress_spread_bps");
    cfg.regime.stress.liquidity_ratio_stress = parse_double(
        get_value(kv, "regime.stress_liquidity_ratio", "3.0"), 3.0, "regime.stress_liquidity_ratio");

    // Chop thresholds
    cfg.regime.chop.adx_max = parse_double(
        get_value(kv, "regime.chop_adx_max", "18.0"), 18.0, "regime.chop_adx_max");

    // Transition / hysteresis policy
    cfg.regime.transition.confirmation_ticks = static_cast<int>(parse_double(
        get_value(kv, "regime.transition_confirmation_ticks", "3"), 3.0, "regime.transition_confirmation_ticks"));
    cfg.regime.transition.min_confidence_to_switch = parse_double(
        get_value(kv, "regime.transition_min_confidence", "0.55"), 0.55, "regime.transition_min_confidence");
    cfg.regime.transition.inertia_alpha = parse_double(
        get_value(kv, "regime.transition_inertia_alpha", "0.15"), 0.15, "regime.transition_inertia_alpha");
    cfg.regime.transition.dwell_time_ticks = static_cast<int>(parse_double(
        get_value(kv, "regime.transition_dwell_ticks", "5"), 5.0, "regime.transition_dwell_ticks"));

    // Confidence policy
    cfg.regime.confidence.base_confidence = parse_double(
        get_value(kv, "regime.confidence_base", "0.5"), 0.5, "regime.confidence_base");
    cfg.regime.confidence.data_quality_weight = parse_double(
        get_value(kv, "regime.confidence_data_quality_weight", "0.2"), 0.2, "regime.confidence_data_quality_weight");
    cfg.regime.confidence.max_indicator_count = static_cast<int>(parse_double(
        get_value(kv, "regime.confidence_max_indicators", "6"), 6.0, "regime.confidence_max_indicators"));
    cfg.regime.confidence.anomaly_confidence = parse_double(
        get_value(kv, "regime.confidence_anomaly", "0.9"), 0.9, "regime.confidence_anomaly");
    cfg.regime.confidence.same_regime_stability = parse_double(
        get_value(kv, "regime.stability_same_regime", "0.9"), 0.9, "regime.stability_same_regime");
    cfg.regime.confidence.first_classification_stability = parse_double(
        get_value(kv, "regime.stability_first_classification", "0.5"), 0.5, "regime.stability_first_classification");

    // ── Секция futures ───────────────────────────────────────────────────
    {
        auto& f = cfg.futures;
        auto futures_enabled_str = get_value(kv, "futures.enabled", "false");
        f.enabled = (futures_enabled_str == "true" || futures_enabled_str == "1");

        f.product_type = get_value(kv, "futures.product_type", f.product_type);
        f.margin_coin  = get_value(kv, "futures.margin_coin", f.margin_coin);
        f.margin_mode  = get_value(kv, "futures.margin_mode", f.margin_mode);

        f.default_leverage = static_cast<int>(parse_double(
            get_value(kv, "futures.default_leverage", std::to_string(f.default_leverage)),
            static_cast<double>(f.default_leverage), "futures.default_leverage"));
        f.max_leverage = static_cast<int>(parse_double(
            get_value(kv, "futures.max_leverage", std::to_string(f.max_leverage)),
            static_cast<double>(f.max_leverage), "futures.max_leverage"));
        f.min_leverage = static_cast<int>(parse_double(
            get_value(kv, "futures.min_leverage", std::to_string(f.min_leverage)),
            static_cast<double>(f.min_leverage), "futures.min_leverage"));

        // Leverage по режиму рынка
        f.leverage_trending = static_cast<int>(parse_double(
            get_value(kv, "futures.leverage_trending", std::to_string(f.leverage_trending)),
            static_cast<double>(f.leverage_trending), "futures.leverage_trending"));
        f.leverage_ranging = static_cast<int>(parse_double(
            get_value(kv, "futures.leverage_ranging", std::to_string(f.leverage_ranging)),
            static_cast<double>(f.leverage_ranging), "futures.leverage_ranging"));
        f.leverage_volatile = static_cast<int>(parse_double(
            get_value(kv, "futures.leverage_volatile", std::to_string(f.leverage_volatile)),
            static_cast<double>(f.leverage_volatile), "futures.leverage_volatile"));
        f.leverage_stress = static_cast<int>(parse_double(
            get_value(kv, "futures.leverage_stress", std::to_string(f.leverage_stress)),
            static_cast<double>(f.leverage_stress), "futures.leverage_stress"));

        // Защита от ликвидации
        f.liquidation_buffer_pct = parse_double(
            get_value(kv, "futures.liquidation_buffer_pct", std::to_string(f.liquidation_buffer_pct)),
            f.liquidation_buffer_pct, "futures.liquidation_buffer_pct");
        f.max_leverage_drawdown_scale = parse_double(
            get_value(kv, "futures.max_leverage_drawdown_scale", std::to_string(f.max_leverage_drawdown_scale)),
            f.max_leverage_drawdown_scale, "futures.max_leverage_drawdown_scale");

        // Фандинг
        f.funding_rate_threshold = parse_double(
            get_value(kv, "futures.funding_rate_threshold", std::to_string(f.funding_rate_threshold)),
            f.funding_rate_threshold, "futures.funding_rate_threshold");
        f.funding_rate_penalty = parse_double(
            get_value(kv, "futures.funding_rate_penalty", std::to_string(f.funding_rate_penalty)),
            f.funding_rate_penalty, "futures.funding_rate_penalty");

        // Maintenance margin rate
        f.maintenance_margin_rate = parse_double(
            get_value(kv, "futures.maintenance_margin_rate", std::to_string(f.maintenance_margin_rate)),
            f.maintenance_margin_rate, "futures.maintenance_margin_rate");

        // Propagate futures mode to pair_selection so PairScanner uses futures endpoints
        if (f.enabled) {
            cfg.pair_selection.futures_enabled = true;
            cfg.pair_selection.product_type = f.product_type;
        }
    }

    // Валидация
    ConfigValidator validator;
    auto vr = validator.validate_detailed(cfg);
    if (!vr.valid) {
        for (const auto& e : vr.errors) {
            std::cerr << "[CONFIG ERROR] " << e << "\n";
        }
        for (const auto& w : vr.warnings) {
            std::cerr << "[CONFIG WARN]  " << w << "\n";
        }
        return Err<AppConfig>(TbError::ConfigValidationFailed);
    }

    return Ok(std::move(cfg));
}

std::unique_ptr<IConfigLoader> create_config_loader() {
    return std::make_unique<YamlConfigLoader>();
}

} // namespace tb::config
