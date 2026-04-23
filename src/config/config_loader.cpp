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
#include <unordered_set>
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
    std::vector<std::pair<int, std::string>> section_stack;

    while (std::getline(stream, line)) {
        // Пропускаем комментарии и пустые строки
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int indent = indent_level(line);
        auto [key, value] = parse_kv_line(trimmed);

        if (key.empty()) continue;

        while (!section_stack.empty() && indent <= section_stack.back().first) {
            section_stack.pop_back();
        }

        if (value.empty()) {
            // Это заголовок секции любого уровня вложенности.
            section_stack.emplace_back(indent, key);
        } else {
            std::string full_key;
            for (const auto& [_, section] : section_stack) {
                if (!full_key.empty()) {
                    full_key += ".";
                }
                full_key += section;
            }
            if (!full_key.empty()) {
                full_key += ".";
            }
            full_key += key;
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
        // Hash computation failed — non-fatal, use sentinel value.
        // Config loading proceeds normally; runtime_manifest will log the hash.
        hash = "hash_computation_failed";
    }

    // Парсим YAML в плоскую карту
    auto kv = parse_yaml_flat(content);

    // N-6: Track consumed keys for unknown-key detection
    std::unordered_set<std::string> consumed_keys;
    auto get_tracked = [&](const std::string& key, const std::string& default_val = "") -> std::string {
        consumed_keys.insert(key);
        return get_value(kv, key, default_val);
    };

    // Заполняем AppConfig из карты
    AppConfig cfg;
    cfg.config_hash = hash;

    // Список ошибок парсинга для fail-fast
    std::vector<std::string> parse_errors;

    // Безопасный парсинг double с fail-fast: при ошибке парсинга добавляет ошибку
    auto parse_double = [&parse_errors](const std::string& s, double def, const char* field_name = "") -> double {
        if (s.empty()) return def; // ключ отсутствует — default ОК
        try { return std::stod(s); }
        catch (...) {
            std::string msg = std::string("Невалидное значение для '") + field_name +
                              "': '" + s + "' (ожидалось число)";
            parse_errors.push_back(msg);
            return def;
        }
    };

    // Безопасный парсинг int с fail-fast
    auto parse_int = [&parse_errors](const std::string& s, int def, const char* field_name) -> int {
        if (s.empty()) return def;
        try { return std::stoi(s); }
        catch (...) {
            std::string msg = std::string("Невалидное значение для '") + field_name +
                              "': '" + s + "' (ожидалось целое число)";
            parse_errors.push_back(msg);
            return def;
        }
    };

    // Универсальный парсинг bool: true/1/yes → true; false/0/no → false; иначе — ошибка
    auto parse_bool = [&parse_errors](const std::string& s, bool def, const char* field_name) -> bool {
        if (s.empty()) return def;
        std::string lower = s;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "true" || lower == "1" || lower == "yes") return true;
        if (lower == "false" || lower == "0" || lower == "no") return false;
        std::string msg = std::string("Невалидное значение для '") + field_name +
                          "': '" + s + "' (ожидалось true/false/1/0/yes/no)";
        parse_errors.push_back(msg);
        return def;
    };

    // Секция trading
    auto mode_str = get_tracked( "trading.mode", "production");
    auto mode = trading_mode_from_string(mode_str);
    // Бот работает ТОЛЬКО в production режиме. Любой другой режим — ошибка.
    if (!mode.has_value()) {
        throw std::runtime_error(
            "Неподдерживаемый trading.mode: '" + mode_str +
            "'. Единственный допустимый режим: production");
    }
    cfg.trading.mode = mode.value();
    cfg.trading.initial_capital = parse_double(
        get_tracked( "trading.initial_capital", "10000.0"), 10000.0, "trading.initial_capital");

    // Секция exchange
    cfg.exchange.endpoint_rest  = get_tracked( "exchange.endpoint_rest",  "https://api.bitget.com");
    cfg.exchange.endpoint_ws    = get_tracked( "exchange.endpoint_ws",    "wss://ws.bitget.com/v2/ws/public");
    cfg.exchange.endpoint_ws_private = get_tracked( "exchange.endpoint_ws_private", "wss://ws.bitget.com/v2/ws/private");
    cfg.exchange.api_key_ref    = get_tracked( "exchange.api_key_ref",    "BITGET_API_KEY");
    cfg.exchange.api_secret_ref = get_tracked( "exchange.api_secret_ref", "BITGET_API_SECRET");
    cfg.exchange.passphrase_ref = get_tracked( "exchange.passphrase_ref", "BITGET_PASSPHRASE");
    auto timeout_str = get_tracked( "exchange.timeout_ms", "5000");
    cfg.exchange.timeout_ms = parse_int(timeout_str, 5000, "exchange.timeout_ms");
    cfg.exchange.use_private_ws = parse_bool(get_tracked( "exchange.use_private_ws", "true"), true, "exchange.use_private_ws");

    // Секция logging
    cfg.logging.level            = get_tracked( "logging.level",           "info");
    cfg.logging.output_path      = get_tracked( "logging.output_path",     "-");
    auto json_str = get_tracked( "logging.structured_json", "false");
    cfg.logging.structured_json  = parse_bool(json_str, false, "logging.structured_json");

    // Секция metrics
    auto metrics_enabled_str = get_tracked( "metrics.enabled", "true");
    cfg.metrics.enabled = parse_bool(metrics_enabled_str, true, "metrics.enabled");
    auto metrics_port_str = get_tracked( "metrics.port", "9090");
    cfg.metrics.port = parse_int(metrics_port_str, 9090, "metrics.port");
    cfg.metrics.path = get_tracked( "metrics.path", "/metrics");

    // Секция risk
    cfg.risk.max_position_notional = parse_double(get_tracked( "risk.max_position_notional", "10000"), 10000.0, "risk.max_position_notional");
    cfg.risk.max_daily_loss_pct    = parse_double(get_tracked( "risk.max_daily_loss_pct",    "2.0"),   2.0, "risk.max_daily_loss_pct");
    cfg.risk.max_drawdown_pct      = parse_double(get_tracked( "risk.max_drawdown_pct",      "5.0"),   5.0, "risk.max_drawdown_pct");
    cfg.risk.max_intraday_drawdown_pct = parse_double(get_tracked( "risk.max_intraday_drawdown_pct", "3.0"), 3.0, "risk.max_intraday_drawdown_pct");
    auto ks_str = get_tracked( "risk.kill_switch_enabled", "true");
    cfg.risk.kill_switch_enabled   = parse_bool(ks_str, true, "risk.kill_switch_enabled");

    // Расширенные параметры риска (desk-grade)
    cfg.risk.max_strategy_daily_loss_pct   = parse_double(get_tracked( "risk.max_strategy_daily_loss_pct",   "1.5"),  1.5, "risk.max_strategy_daily_loss_pct");
    cfg.risk.max_strategy_exposure_pct     = parse_double(get_tracked( "risk.max_strategy_exposure_pct",     "30.0"), 30.0, "risk.max_strategy_exposure_pct");
    cfg.risk.max_symbol_concentration_pct  = parse_double(get_tracked( "risk.max_symbol_concentration_pct",  "35.0"), 35.0, "risk.max_symbol_concentration_pct");
    auto sdp_str = get_tracked( "risk.max_same_direction_positions", "3");
    cfg.risk.max_same_direction_positions = parse_int(sdp_str, 3, "risk.max_same_direction_positions");
    auto ral_str = get_tracked( "risk.regime_aware_limits_enabled", "true");
    cfg.risk.regime_aware_limits_enabled   = parse_bool(ral_str, true, "risk.regime_aware_limits_enabled");
    cfg.risk.stress_regime_scale           = parse_double(get_tracked( "risk.stress_regime_scale",           "0.5"),  0.5, "risk.stress_regime_scale");
    cfg.risk.trending_regime_scale         = parse_double(get_tracked( "risk.trending_regime_scale",         "1.2"),  1.2, "risk.trending_regime_scale");
    cfg.risk.chop_regime_scale             = parse_double(get_tracked( "risk.chop_regime_scale",             "0.7"),  0.7, "risk.chop_regime_scale");
    auto tph_str = get_tracked( "risk.max_trades_per_hour", "8");
    cfg.risk.max_trades_per_hour = parse_int(tph_str, 8, "risk.max_trades_per_hour");
    cfg.risk.min_trade_interval_sec        = parse_double(get_tracked( "risk.min_trade_interval_sec",        "30.0"), 30.0, "risk.min_trade_interval_sec");
    cfg.risk.max_adverse_excursion_pct     = parse_double(get_tracked( "risk.max_adverse_excursion_pct",     "3.0"),  3.0, "risk.max_adverse_excursion_pct");
    cfg.risk.max_realized_daily_loss_pct   = parse_double(get_tracked( "risk.max_realized_daily_loss_pct",   "1.5"),  1.5, "risk.max_realized_daily_loss_pct");
    auto cutoff_str = get_tracked( "risk.utc_cutoff_hour", "-1");
    cfg.risk.utc_cutoff_hour = parse_int(cutoff_str, -1, "risk.utc_cutoff_hour");

    // Секция pair_selection
    auto ps_mode_str = get_tracked( "pair_selection.mode", "auto");
    cfg.pair_selection.mode = (ps_mode_str == "manual")
        ? PairSelectionMode::Manual : PairSelectionMode::Auto;
    auto top_n_str = get_tracked( "pair_selection.top_n", "5");
    cfg.pair_selection.top_n = parse_int(top_n_str, 5, "pair_selection.top_n");
    cfg.pair_selection.min_volume_usdt = parse_double(
        get_tracked( "pair_selection.min_volume_usdt", "500000"), 500000.0, "pair_selection.min_volume_usdt");
    cfg.pair_selection.max_spread_bps = parse_double(
        get_tracked( "pair_selection.max_spread_bps", "50"), 50.0, "pair_selection.max_spread_bps");
    auto rotation_str = get_tracked( "pair_selection.rotation_interval_hours", "24");
    cfg.pair_selection.rotation_interval_hours = parse_int(rotation_str, 24, "pair_selection.rotation_interval_hours");

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
        get_tracked( "pair_selection.symbols", ""));
    cfg.pair_selection.blacklist = parse_list(
        get_tracked( "pair_selection.blacklist", ""));

    // Расширенные настройки pair_selection
    cfg.pair_selection.max_candidates_for_candles = parse_int(
        get_tracked( "pair_selection.max_candidates_for_candles", "30"), 30, "pair_selection.max_candidates_for_candles");
    cfg.pair_selection.scan_timeout_ms = parse_int(
        get_tracked( "pair_selection.scan_timeout_ms", "60000"), 60000, "pair_selection.scan_timeout_ms");
    cfg.pair_selection.api_retry_max = parse_int(
        get_tracked( "pair_selection.api_retry_max", "3"), 3, "pair_selection.api_retry_max");
    cfg.pair_selection.api_retry_base_delay_ms = parse_int(
        get_tracked( "pair_selection.api_retry_base_delay_ms", "200"), 200, "pair_selection.api_retry_base_delay_ms");
    cfg.pair_selection.circuit_breaker_threshold = parse_int(
        get_tracked( "pair_selection.circuit_breaker_threshold", "5"), 5, "pair_selection.circuit_breaker_threshold");
    cfg.pair_selection.circuit_breaker_reset_ms = parse_int(
        get_tracked( "pair_selection.circuit_breaker_reset_ms", "300000"), 300000, "pair_selection.circuit_breaker_reset_ms");
    cfg.pair_selection.max_correlation_in_basket = parse_double(
        get_tracked( "pair_selection.max_correlation_in_basket", "0.85"), 0.85, "pair_selection.max_correlation_in_basket");
    cfg.pair_selection.max_pairs_per_sector = parse_int(
        get_tracked( "pair_selection.max_pairs_per_sector", "2"), 2, "pair_selection.max_pairs_per_sector");
    cfg.pair_selection.min_liquidity_depth_usdt = parse_double(
        get_tracked( "pair_selection.min_liquidity_depth_usdt", "50000"), 50000.0, "pair_selection.min_liquidity_depth_usdt");
    cfg.pair_selection.enable_diversification = parse_bool(
        get_tracked( "pair_selection.enable_diversification", "true"), true, "pair_selection.enable_diversification");

    // ScorerConfig (вложенный в pair_selection) — только живые поля, влияющие на runtime
    auto& sc = cfg.pair_selection.scorer;
    sc.volume_tier_minimal = parse_double(get_tracked( "pair_selection.scorer_volume_tier_minimal", "100000"), 100'000.0, "pair_selection.scorer_volume_tier_minimal");
    sc.volatility_low_threshold = parse_double(get_tracked( "pair_selection.scorer_volatility_low_threshold", "0.5"), 0.5, "pair_selection.scorer_volatility_low_threshold");
    sc.volatility_high_threshold = parse_double(get_tracked( "pair_selection.scorer_volatility_high_threshold", "20.0"), 20.0, "pair_selection.scorer_volatility_high_threshold");
    sc.filter_min_change_24h = parse_double(get_tracked( "pair_selection.scorer_filter_min_change_24h", "-1.0"), -1.0, "pair_selection.scorer_filter_min_change_24h");
    sc.filter_max_change_24h = parse_double(get_tracked( "pair_selection.scorer_filter_max_change_24h", "20.0"), 20.0, "pair_selection.scorer_filter_max_change_24h");

    // ============================================================
    // Decision Engine
    // ============================================================
    cfg.decision.min_conviction_threshold = parse_double(
        get_tracked( "decision.min_conviction_threshold", "0.45"), 0.45, "decision.min_conviction_threshold");
    cfg.decision.conflict_dominance_threshold = parse_double(
        get_tracked( "decision.conflict_dominance_threshold", "0.60"), 0.60, "decision.conflict_dominance_threshold");

    // Advanced decision features
    cfg.decision.enable_regime_threshold_scaling = parse_bool(
        get_tracked( "decision.enable_regime_threshold_scaling", "true"), true, "decision.enable_regime_threshold_scaling");
    cfg.decision.enable_regime_dominance_scaling = parse_bool(
        get_tracked( "decision.enable_regime_dominance_scaling", "true"), true, "decision.enable_regime_dominance_scaling");
    cfg.decision.enable_time_decay = parse_bool(
        get_tracked( "decision.enable_time_decay", "true"), true, "decision.enable_time_decay");
    cfg.decision.time_decay_halflife_ms = parse_double(
        get_tracked( "decision.time_decay_halflife_ms", "500"), 500.0, "decision.time_decay_halflife_ms");
    cfg.decision.enable_ensemble_conviction = parse_bool(
        get_tracked( "decision.enable_ensemble_conviction", "true"), true, "decision.enable_ensemble_conviction");
    cfg.decision.ensemble_agreement_bonus = parse_double(
        get_tracked( "decision.ensemble_agreement_bonus", "0.06"), 0.06, "decision.ensemble_agreement_bonus");
    cfg.decision.ensemble_max_bonus = parse_double(
        get_tracked( "decision.ensemble_max_bonus", "0.15"), 0.15, "decision.ensemble_max_bonus");
    cfg.decision.enable_portfolio_awareness = parse_bool(
        get_tracked( "decision.enable_portfolio_awareness", "true"), true, "decision.enable_portfolio_awareness");
    cfg.decision.drawdown_boost_scale = parse_double(
        get_tracked( "decision.drawdown_boost_scale", "0.02"), 0.02, "decision.drawdown_boost_scale");
    cfg.decision.drawdown_max_boost = parse_double(
        get_tracked( "decision.drawdown_max_boost", "0.08"), 0.08, "decision.drawdown_max_boost");
    cfg.decision.consecutive_loss_boost = parse_double(
        get_tracked( "decision.consecutive_loss_boost", "0.005"), 0.005, "decision.consecutive_loss_boost");
    cfg.decision.enable_execution_cost_modeling = parse_bool(
        get_tracked( "decision.enable_execution_cost_modeling", "true"), true, "decision.enable_execution_cost_modeling");
    cfg.decision.max_acceptable_cost_bps = parse_double(
        get_tracked( "decision.max_acceptable_cost_bps", "50"), 50.0, "decision.max_acceptable_cost_bps");
    cfg.decision.enable_time_skew_detection = parse_bool(
        get_tracked( "decision.enable_time_skew_detection", "true"), true, "decision.enable_time_skew_detection");

    // ============================================================
    // Trading Params (position management)
    // Defaults match TradingParamsConfig struct for USDT-M futures scalping.
    // ============================================================
    cfg.trading_params.atr_stop_multiplier = parse_double(
        get_tracked( "trading_params.atr_stop_multiplier", "2.0"), 2.0, "trading_params.atr_stop_multiplier");
    cfg.trading_params.max_loss_per_trade_pct = parse_double(
        get_tracked( "trading_params.max_loss_per_trade_pct", "1.0"), 1.0, "trading_params.max_loss_per_trade_pct");
    cfg.trading_params.price_stop_loss_pct = parse_double(
        get_tracked( "trading_params.price_stop_loss_pct", "1.5"), 1.5, "trading_params.price_stop_loss_pct");
    cfg.trading_params.min_risk_reward_ratio = parse_double(
        get_tracked( "trading_params.min_risk_reward_ratio", "0.8"), 0.8, "trading_params.min_risk_reward_ratio");
    cfg.trading_params.breakeven_atr_threshold = parse_double(
        get_tracked( "trading_params.breakeven_atr_threshold", "0.8"), 0.8, "trading_params.breakeven_atr_threshold");
    cfg.trading_params.partial_tp_atr_threshold = parse_double(
        get_tracked( "trading_params.partial_tp_atr_threshold", "2.0"), 2.0, "trading_params.partial_tp_atr_threshold");
    cfg.trading_params.partial_tp_fraction = parse_double(
        get_tracked( "trading_params.partial_tp_fraction", "0.5"), 0.5, "trading_params.partial_tp_fraction");
    cfg.trading_params.order_cooldown_seconds = static_cast<int>(parse_double(
        get_tracked( "trading_params.order_cooldown_seconds", "10"), 10, "trading_params.order_cooldown_seconds"));
    cfg.trading_params.stop_loss_cooldown_seconds = static_cast<int>(parse_double(
        get_tracked( "trading_params.stop_loss_cooldown_seconds", "180"), 180, "trading_params.stop_loss_cooldown_seconds"));
    cfg.trading_params.dust_threshold_usdt = parse_double(
        get_tracked( "trading_params.dust_threshold_usdt", "0.50"), 0.50, "trading_params.dust_threshold_usdt");
    cfg.trading_params.quick_profit_fee_multiplier = parse_double(
        get_tracked( "trading_params.quick_profit_fee_multiplier", "5.0"), 5.0, "trading_params.quick_profit_fee_multiplier");
    cfg.trading_params.pnl_gate_loss_pct = parse_double(
        get_tracked( "trading_params.pnl_gate_loss_pct", "0.5"), 0.5, "trading_params.pnl_gate_loss_pct");

    // ── Hedge Recovery ──
    cfg.trading_params.hedge_recovery_enabled =
        get_tracked( "trading_params.hedge_recovery_enabled", "false") == "true";
    cfg.trading_params.hedge_trigger_loss_pct = parse_double(
        get_tracked( "trading_params.hedge_trigger_loss_pct", "1.5"), 1.5, "trading_params.hedge_trigger_loss_pct");
    cfg.trading_params.hedge_profit_close_fee_mult = parse_double(
        get_tracked( "trading_params.hedge_profit_close_fee_mult", "2.0"), 2.0, "trading_params.hedge_profit_close_fee_mult");

    // ── Operational Safety ────────────────────────────────────────────────
    cfg.operational_safety.feed_stale_ms = static_cast<int>(parse_double(
        get_tracked( "operational_safety.feed_stale_ms", "1200"), 1200, "operational_safety.feed_stale_ms"));
    cfg.operational_safety.private_ws_gap_ms = static_cast<int>(parse_double(
        get_tracked( "operational_safety.private_ws_gap_ms", "2500"), 2500, "operational_safety.private_ws_gap_ms"));
    cfg.operational_safety.order_state_desync_ms = static_cast<int>(parse_double(
        get_tracked( "operational_safety.order_state_desync_ms", "5000"), 5000, "operational_safety.order_state_desync_ms"));
    cfg.operational_safety.orphan_leg_grace_ms = static_cast<int>(parse_double(
        get_tracked( "operational_safety.orphan_leg_grace_ms", "250"), 250, "operational_safety.orphan_leg_grace_ms"));
    cfg.operational_safety.block_trade_until_full_sync =
        get_tracked( "operational_safety.block_trade_until_full_sync", "true") == "true";
    cfg.operational_safety.venue_degraded_flatten_ms = static_cast<int>(parse_double(
        get_tracked( "operational_safety.venue_degraded_flatten_ms", "8000"), 8000, "operational_safety.venue_degraded_flatten_ms"));
    cfg.operational_safety.operational_deadman_minutes = static_cast<int>(parse_double(
        get_tracked( "operational_safety.operational_deadman_minutes", "45"), 45, "operational_safety.operational_deadman_minutes"));

    // ── Исполнительная альфа (execution_alpha) ────────────────────────────
    cfg.execution_alpha.max_spread_bps_passive = parse_double(
        get_tracked( "execution_alpha.max_spread_bps_passive", "15.0"), 15.0, "execution_alpha.max_spread_bps_passive");
    cfg.execution_alpha.max_spread_bps_any = parse_double(
        get_tracked( "execution_alpha.max_spread_bps_any", "50.0"), 50.0, "execution_alpha.max_spread_bps_any");
    cfg.execution_alpha.adverse_selection_threshold = parse_double(
        get_tracked( "execution_alpha.adverse_selection_threshold", "0.7"), 0.7, "execution_alpha.adverse_selection_threshold");
    cfg.execution_alpha.urgency_passive_threshold = parse_double(
        get_tracked( "execution_alpha.urgency_passive_threshold", "0.5"), 0.5, "execution_alpha.urgency_passive_threshold");
    cfg.execution_alpha.urgency_aggressive_threshold = parse_double(
        get_tracked( "execution_alpha.urgency_aggressive_threshold", "0.8"), 0.8, "execution_alpha.urgency_aggressive_threshold");
    cfg.execution_alpha.large_order_slice_threshold = parse_double(
        get_tracked( "execution_alpha.large_order_slice_threshold", "0.1"), 0.1, "execution_alpha.large_order_slice_threshold");
    cfg.execution_alpha.vpin_toxic_threshold = parse_double(
        get_tracked( "execution_alpha.vpin_toxic_threshold", "0.65"), 0.65, "execution_alpha.vpin_toxic_threshold");
    cfg.execution_alpha.vpin_weight = parse_double(
        get_tracked( "execution_alpha.vpin_weight", "0.40"), 0.40, "execution_alpha.vpin_weight");
    cfg.execution_alpha.imbalance_favorable_threshold = parse_double(
        get_tracked( "execution_alpha.imbalance_favorable_threshold", "0.30"), 0.30, "execution_alpha.imbalance_favorable_threshold");
    cfg.execution_alpha.imbalance_unfavorable_threshold = parse_double(
        get_tracked( "execution_alpha.imbalance_unfavorable_threshold", "0.30"), 0.30, "execution_alpha.imbalance_unfavorable_threshold");
    cfg.execution_alpha.use_weighted_mid_price = parse_bool(
        get_tracked( "execution_alpha.use_weighted_mid_price", "true"), true, "execution_alpha.use_weighted_mid_price");
    cfg.execution_alpha.limit_price_passive_bps = parse_double(
        get_tracked( "execution_alpha.limit_price_passive_bps", "3.0"), 3.0, "execution_alpha.limit_price_passive_bps");
    cfg.execution_alpha.urgency_cusum_boost = parse_double(
        get_tracked( "execution_alpha.urgency_cusum_boost", "0.15"), 0.15, "execution_alpha.urgency_cusum_boost");
    cfg.execution_alpha.urgency_tod_weight = parse_double(
        get_tracked( "execution_alpha.urgency_tod_weight", "0.10"), 0.10, "execution_alpha.urgency_tod_weight");
    cfg.execution_alpha.min_fill_probability_passive = parse_double(
        get_tracked( "execution_alpha.min_fill_probability_passive", "0.25"), 0.25, "execution_alpha.min_fill_probability_passive");
    cfg.execution_alpha.postonly_spread_threshold_bps = parse_double(
        get_tracked( "execution_alpha.postonly_spread_threshold_bps", "4.5"), 4.5, "execution_alpha.postonly_spread_threshold_bps");
    cfg.execution_alpha.postonly_urgency_max = parse_double(
        get_tracked( "execution_alpha.postonly_urgency_max", "0.35"), 0.35, "execution_alpha.postonly_urgency_max");
    cfg.execution_alpha.postonly_adverse_max = parse_double(
        get_tracked( "execution_alpha.postonly_adverse_max", "0.35"), 0.35, "execution_alpha.postonly_adverse_max");
    cfg.execution_alpha.taker_fee_bps = parse_double(
        get_tracked( "execution_alpha.taker_fee_bps", "6.0"), 6.0, "execution_alpha.taker_fee_bps");
    cfg.execution_alpha.maker_fee_bps = parse_double(
        get_tracked( "execution_alpha.maker_fee_bps", "2.0"), 2.0, "execution_alpha.maker_fee_bps");
    cfg.execution_alpha.opportunity_cost_bps = parse_double(
        get_tracked( "execution_alpha.opportunity_cost_bps", "30.0"), 30.0, "execution_alpha.opportunity_cost_bps");
    cfg.execution_alpha.queue_depletion_penalty = parse_double(
        get_tracked( "execution_alpha.queue_depletion_penalty", "0.08"), 0.08, "execution_alpha.queue_depletion_penalty");
    cfg.execution_alpha.churn_penalty = parse_double(
        get_tracked( "execution_alpha.churn_penalty", "0.06"), 0.06, "execution_alpha.churn_penalty");
    cfg.execution_alpha.feedback_weight = parse_double(
        get_tracked( "execution_alpha.feedback_weight", "0.30"), 0.30, "execution_alpha.feedback_weight");

    // ── Opportunity Cost ──────────────────────────────────────────────────
    cfg.opportunity_cost.min_net_expected_bps = parse_double(
        get_tracked( "opportunity_cost.min_net_expected_bps", "1.0"), 1.0, "opportunity_cost.min_net_expected_bps");
    cfg.opportunity_cost.execute_min_net_bps = parse_double(
        get_tracked( "opportunity_cost.execute_min_net_bps", "3.0"), 3.0, "opportunity_cost.execute_min_net_bps");
    cfg.opportunity_cost.high_exposure_threshold = parse_double(
        get_tracked( "opportunity_cost.high_exposure_threshold", "0.70"), 0.70, "opportunity_cost.high_exposure_threshold");
    cfg.opportunity_cost.high_exposure_min_conviction = parse_double(
        get_tracked( "opportunity_cost.high_exposure_min_conviction", "0.60"), 0.60, "opportunity_cost.high_exposure_min_conviction");
    cfg.opportunity_cost.max_symbol_concentration = parse_double(
        get_tracked( "opportunity_cost.max_symbol_concentration", "0.25"), 0.25, "opportunity_cost.max_symbol_concentration");
    cfg.opportunity_cost.max_strategy_concentration = parse_double(
        get_tracked( "opportunity_cost.max_strategy_concentration", "0.35"), 0.35, "opportunity_cost.max_strategy_concentration");
    cfg.opportunity_cost.capital_exhaustion_threshold = parse_double(
        get_tracked( "opportunity_cost.capital_exhaustion_threshold", "0.85"), 0.85, "opportunity_cost.capital_exhaustion_threshold");
    cfg.opportunity_cost.weight_conviction = parse_double(
        get_tracked( "opportunity_cost.weight_conviction", "0.35"), 0.35, "opportunity_cost.weight_conviction");
    cfg.opportunity_cost.weight_net_edge = parse_double(
        get_tracked( "opportunity_cost.weight_net_edge", "0.35"), 0.35, "opportunity_cost.weight_net_edge");
    cfg.opportunity_cost.weight_capital_efficiency = parse_double(
        get_tracked( "opportunity_cost.weight_capital_efficiency", "0.15"), 0.15, "opportunity_cost.weight_capital_efficiency");
    cfg.opportunity_cost.weight_urgency = parse_double(
        get_tracked( "opportunity_cost.weight_urgency", "0.15"), 0.15, "opportunity_cost.weight_urgency");
    cfg.opportunity_cost.conviction_to_bps_scale = parse_double(
        get_tracked( "opportunity_cost.conviction_to_bps_scale", "100.0"), 100.0, "opportunity_cost.conviction_to_bps_scale");
    cfg.opportunity_cost.upgrade_min_edge_advantage_bps = parse_double(
        get_tracked( "opportunity_cost.upgrade_min_edge_advantage_bps", "5.0"), 5.0, "opportunity_cost.upgrade_min_edge_advantage_bps");
    cfg.opportunity_cost.drawdown_penalty_scale = parse_double(
        get_tracked( "opportunity_cost.drawdown_penalty_scale", "0.5"), 0.5, "opportunity_cost.drawdown_penalty_scale");
    cfg.opportunity_cost.consecutive_loss_penalty = parse_double(
        get_tracked( "opportunity_cost.consecutive_loss_penalty", "0.02"), 0.02, "opportunity_cost.consecutive_loss_penalty");

    // ── Секция regime ──
    // Trend thresholds
    cfg.regime.trend.adx_strong = parse_double(
        get_tracked( "regime.trend_adx_strong", "30.0"), 30.0, "regime.trend_adx_strong");
    cfg.regime.trend.adx_weak_min = parse_double(
        get_tracked( "regime.trend_adx_weak_min", "18.0"), 18.0, "regime.trend_adx_weak_min");
    cfg.regime.trend.adx_weak_max = parse_double(
        get_tracked( "regime.trend_adx_weak_max", "30.0"), 30.0, "regime.trend_adx_weak_max");
    cfg.regime.trend.rsi_trend_bias = parse_double(
        get_tracked( "regime.trend_rsi_bias", "55.0"), 55.0, "regime.trend_rsi_bias");

    // Mean-reversion thresholds
    cfg.regime.mean_reversion.rsi_overbought = parse_double(
        get_tracked( "regime.mr_rsi_overbought", "70.0"), 70.0, "regime.mr_rsi_overbought");
    cfg.regime.mean_reversion.rsi_oversold = parse_double(
        get_tracked( "regime.mr_rsi_oversold", "30.0"), 30.0, "regime.mr_rsi_oversold");
    cfg.regime.mean_reversion.adx_max = parse_double(
        get_tracked( "regime.mr_adx_max", "25.0"), 25.0, "regime.mr_adx_max");

    // Volatility thresholds
    cfg.regime.volatility.bb_bandwidth_expansion = parse_double(
        get_tracked( "regime.vol_bb_expansion", "0.06"), 0.06, "regime.vol_bb_expansion");
    cfg.regime.volatility.bb_bandwidth_compression = parse_double(
        get_tracked( "regime.vol_bb_compression", "0.02"), 0.02, "regime.vol_bb_compression");
    cfg.regime.volatility.atr_norm_expansion = parse_double(
        get_tracked( "regime.vol_atr_expansion", "0.02"), 0.02, "regime.vol_atr_expansion");
    cfg.regime.volatility.adx_compression_max = parse_double(
        get_tracked( "regime.vol_adx_compression_max", "20.0"), 20.0, "regime.vol_adx_compression_max");

    // Stress thresholds
    cfg.regime.stress.rsi_extreme_high = parse_double(
        get_tracked( "regime.stress_rsi_extreme_high", "85.0"), 85.0, "regime.stress_rsi_extreme_high");
    cfg.regime.stress.rsi_extreme_low = parse_double(
        get_tracked( "regime.stress_rsi_extreme_low", "15.0"), 15.0, "regime.stress_rsi_extreme_low");
    cfg.regime.stress.obv_norm_extreme = parse_double(
        get_tracked( "regime.stress_obv_extreme", "2.0"), 2.0, "regime.stress_obv_extreme");
    cfg.regime.stress.aggressive_flow_toxic = parse_double(
        get_tracked( "regime.stress_aggressive_flow", "0.75"), 0.75, "regime.stress_aggressive_flow");
    cfg.regime.stress.spread_toxic_bps = parse_double(
        get_tracked( "regime.stress_spread_toxic_bps", "15.0"), 15.0, "regime.stress_spread_toxic_bps");
    cfg.regime.stress.book_instability_threshold = parse_double(
        get_tracked( "regime.stress_book_instability", "0.6"), 0.6, "regime.stress_book_instability");
    cfg.regime.stress.spread_stress_bps = parse_double(
        get_tracked( "regime.stress_spread_bps", "30.0"), 30.0, "regime.stress_spread_bps");
    cfg.regime.stress.liquidity_ratio_stress = parse_double(
        get_tracked( "regime.stress_liquidity_ratio", "0.3"), 0.3, "regime.stress_liquidity_ratio");

    // Chop thresholds
    cfg.regime.chop.adx_max = parse_double(
        get_tracked( "regime.chop_adx_max", "18.0"), 18.0, "regime.chop_adx_max");

    // Transition / hysteresis policy
    cfg.regime.transition.confirmation_ticks = static_cast<int>(parse_double(
        get_tracked( "regime.transition_confirmation_ticks", "3"), 3.0, "regime.transition_confirmation_ticks"));
    cfg.regime.transition.min_confidence_to_switch = parse_double(
        get_tracked( "regime.transition_min_confidence", "0.55"), 0.55, "regime.transition_min_confidence");
    cfg.regime.transition.dwell_time_ticks = static_cast<int>(parse_double(
        get_tracked( "regime.transition_dwell_ticks", "5"), 5.0, "regime.transition_dwell_ticks"));

    // Confidence policy
    cfg.regime.confidence.base_confidence = parse_double(
        get_tracked( "regime.confidence_base", "0.5"), 0.5, "regime.confidence_base");
    cfg.regime.confidence.data_quality_weight = parse_double(
        get_tracked( "regime.confidence_data_quality_weight", "0.2"), 0.2, "regime.confidence_data_quality_weight");
    cfg.regime.confidence.max_indicator_count = static_cast<int>(parse_double(
        get_tracked( "regime.confidence_max_indicators", "12"), 12.0, "regime.confidence_max_indicators"));
    cfg.regime.confidence.anomaly_confidence = parse_double(
        get_tracked( "regime.confidence_anomaly", "0.9"), 0.9, "regime.confidence_anomaly");
    cfg.regime.confidence.same_regime_stability = parse_double(
        get_tracked( "regime.stability_same_regime", "0.9"), 0.9, "regime.stability_same_regime");
    cfg.regime.confidence.first_classification_stability = parse_double(
        get_tracked( "regime.stability_first_classification", "0.5"), 0.5, "regime.stability_first_classification");

    // ── Секция world_model ───────────────────────────────────────────────
    cfg.world_model.model_version = get_tracked(
        "world_model.model_version", cfg.world_model.model_version);
    cfg.world_model.min_valid_indicators = static_cast<int>(parse_double(
        get_tracked( "world_model.min_valid_indicators", std::to_string(cfg.world_model.min_valid_indicators)),
        static_cast<double>(cfg.world_model.min_valid_indicators), "world_model.min_valid_indicators"));

    cfg.world_model.toxic.book_instability_min = parse_double(
        get_tracked( "world_model.toxic_microstructure.book_instability_min", std::to_string(cfg.world_model.toxic.book_instability_min)),
        cfg.world_model.toxic.book_instability_min, "world_model.toxic_microstructure.book_instability_min");
    cfg.world_model.toxic.aggressive_flow_min = parse_double(
        get_tracked( "world_model.toxic_microstructure.aggressive_flow_min", std::to_string(cfg.world_model.toxic.aggressive_flow_min)),
        cfg.world_model.toxic.aggressive_flow_min, "world_model.toxic_microstructure.aggressive_flow_min");
    cfg.world_model.toxic.spread_bps_min = parse_double(
        get_tracked( "world_model.toxic_microstructure.spread_bps_min", std::to_string(cfg.world_model.toxic.spread_bps_min)),
        cfg.world_model.toxic.spread_bps_min, "world_model.toxic_microstructure.spread_bps_min");

    cfg.world_model.liquidity_vacuum.spread_bps_critical = parse_double(
        get_tracked( "world_model.liquidity_vacuum.spread_bps_critical", std::to_string(cfg.world_model.liquidity_vacuum.spread_bps_critical)),
        cfg.world_model.liquidity_vacuum.spread_bps_critical, "world_model.liquidity_vacuum.spread_bps_critical");
    cfg.world_model.liquidity_vacuum.spread_bps_secondary = parse_double(
        get_tracked( "world_model.liquidity_vacuum.spread_bps_secondary", std::to_string(cfg.world_model.liquidity_vacuum.spread_bps_secondary)),
        cfg.world_model.liquidity_vacuum.spread_bps_secondary, "world_model.liquidity_vacuum.spread_bps_secondary");
    cfg.world_model.liquidity_vacuum.liquidity_ratio_min = parse_double(
        get_tracked( "world_model.liquidity_vacuum.liquidity_ratio_min", std::to_string(cfg.world_model.liquidity_vacuum.liquidity_ratio_min)),
        cfg.world_model.liquidity_vacuum.liquidity_ratio_min, "world_model.liquidity_vacuum.liquidity_ratio_min");

    cfg.world_model.exhaustion.rsi_upper = parse_double(
        get_tracked( "world_model.exhaustion_spike.rsi_upper", std::to_string(cfg.world_model.exhaustion.rsi_upper)),
        cfg.world_model.exhaustion.rsi_upper, "world_model.exhaustion_spike.rsi_upper");
    cfg.world_model.exhaustion.rsi_lower = parse_double(
        get_tracked( "world_model.exhaustion_spike.rsi_lower", std::to_string(cfg.world_model.exhaustion.rsi_lower)),
        cfg.world_model.exhaustion.rsi_lower, "world_model.exhaustion_spike.rsi_lower");
    cfg.world_model.exhaustion.momentum_abs_min = parse_double(
        get_tracked( "world_model.exhaustion_spike.momentum_abs_min", std::to_string(cfg.world_model.exhaustion.momentum_abs_min)),
        cfg.world_model.exhaustion.momentum_abs_min, "world_model.exhaustion_spike.momentum_abs_min");

    cfg.world_model.fragile_breakout.bb_percent_b_upper = parse_double(
        get_tracked( "world_model.fragile_breakout.bb_percent_b_upper", std::to_string(cfg.world_model.fragile_breakout.bb_percent_b_upper)),
        cfg.world_model.fragile_breakout.bb_percent_b_upper, "world_model.fragile_breakout.bb_percent_b_upper");
    cfg.world_model.fragile_breakout.bb_percent_b_lower = parse_double(
        get_tracked( "world_model.fragile_breakout.bb_percent_b_lower", std::to_string(cfg.world_model.fragile_breakout.bb_percent_b_lower)),
        cfg.world_model.fragile_breakout.bb_percent_b_lower, "world_model.fragile_breakout.bb_percent_b_lower");
    cfg.world_model.fragile_breakout.volatility_5_min = parse_double(
        get_tracked( "world_model.fragile_breakout.volatility_5_min", std::to_string(cfg.world_model.fragile_breakout.volatility_5_min)),
        cfg.world_model.fragile_breakout.volatility_5_min, "world_model.fragile_breakout.volatility_5_min");
    cfg.world_model.fragile_breakout.book_imbalance_abs_min = parse_double(
        get_tracked( "world_model.fragile_breakout.book_imbalance_abs_min", std::to_string(cfg.world_model.fragile_breakout.book_imbalance_abs_min)),
        cfg.world_model.fragile_breakout.book_imbalance_abs_min, "world_model.fragile_breakout.book_imbalance_abs_min");

    cfg.world_model.compression.bb_bandwidth_max = parse_double(
        get_tracked( "world_model.compression.bb_bandwidth_max", std::to_string(cfg.world_model.compression.bb_bandwidth_max)),
        cfg.world_model.compression.bb_bandwidth_max, "world_model.compression.bb_bandwidth_max");
    cfg.world_model.compression.atr_normalized_max = parse_double(
        get_tracked( "world_model.compression.atr_normalized_max", std::to_string(cfg.world_model.compression.atr_normalized_max)),
        cfg.world_model.compression.atr_normalized_max, "world_model.compression.atr_normalized_max");
    cfg.world_model.compression.volatility_5_max = parse_double(
        get_tracked( "world_model.compression.volatility_5_max", std::to_string(cfg.world_model.compression.volatility_5_max)),
        cfg.world_model.compression.volatility_5_max, "world_model.compression.volatility_5_max");

    cfg.world_model.stable_trend.adx_min = parse_double(
        get_tracked( "world_model.stable_trend.adx_min", std::to_string(cfg.world_model.stable_trend.adx_min)),
        cfg.world_model.stable_trend.adx_min, "world_model.stable_trend.adx_min");
    cfg.world_model.stable_trend.rsi_lower = parse_double(
        get_tracked( "world_model.stable_trend.rsi_lower", std::to_string(cfg.world_model.stable_trend.rsi_lower)),
        cfg.world_model.stable_trend.rsi_lower, "world_model.stable_trend.rsi_lower");
    cfg.world_model.stable_trend.rsi_upper = parse_double(
        get_tracked( "world_model.stable_trend.rsi_upper", std::to_string(cfg.world_model.stable_trend.rsi_upper)),
        cfg.world_model.stable_trend.rsi_upper, "world_model.stable_trend.rsi_upper");

    cfg.world_model.chop_noise.adx_max = parse_double(
        get_tracked( "world_model.chop_noise.adx_max", std::to_string(cfg.world_model.chop_noise.adx_max)),
        cfg.world_model.chop_noise.adx_max, "world_model.chop_noise.adx_max");
    cfg.world_model.chop_noise.rsi_lower = parse_double(
        get_tracked( "world_model.chop_noise.rsi_lower", std::to_string(cfg.world_model.chop_noise.rsi_lower)),
        cfg.world_model.chop_noise.rsi_lower, "world_model.chop_noise.rsi_lower");
    cfg.world_model.chop_noise.rsi_upper = parse_double(
        get_tracked( "world_model.chop_noise.rsi_upper", std::to_string(cfg.world_model.chop_noise.rsi_upper)),
        cfg.world_model.chop_noise.rsi_upper, "world_model.chop_noise.rsi_upper");
    cfg.world_model.chop_noise.spread_bps_max = parse_double(
        get_tracked( "world_model.chop_noise.spread_bps_max", std::to_string(cfg.world_model.chop_noise.spread_bps_max)),
        cfg.world_model.chop_noise.spread_bps_max, "world_model.chop_noise.spread_bps_max");

    cfg.world_model.fragility.spread_stress_weight = parse_double(
        get_tracked( "world_model.fragility.spread_stress_weight", std::to_string(cfg.world_model.fragility.spread_stress_weight)),
        cfg.world_model.fragility.spread_stress_weight, "world_model.fragility.spread_stress_weight");
    cfg.world_model.fragility.book_instability_weight = parse_double(
        get_tracked( "world_model.fragility.book_instability_weight", std::to_string(cfg.world_model.fragility.book_instability_weight)),
        cfg.world_model.fragility.book_instability_weight, "world_model.fragility.book_instability_weight");
    cfg.world_model.fragility.volatility_accel_weight = parse_double(
        get_tracked( "world_model.fragility.volatility_accel_weight", std::to_string(cfg.world_model.fragility.volatility_accel_weight)),
        cfg.world_model.fragility.volatility_accel_weight, "world_model.fragility.volatility_accel_weight");
    cfg.world_model.fragility.liquidity_imbalance_weight = parse_double(
        get_tracked( "world_model.fragility.liquidity_imbalance_weight", std::to_string(cfg.world_model.fragility.liquidity_imbalance_weight)),
        cfg.world_model.fragility.liquidity_imbalance_weight, "world_model.fragility.liquidity_imbalance_weight");
    cfg.world_model.fragility.transition_instability_weight = parse_double(
        get_tracked( "world_model.fragility.transition_instability_weight", std::to_string(cfg.world_model.fragility.transition_instability_weight)),
        cfg.world_model.fragility.transition_instability_weight, "world_model.fragility.transition_instability_weight");
    cfg.world_model.fragility.vpin_toxicity_weight = parse_double(
        get_tracked( "world_model.fragility.vpin_toxicity_weight", std::to_string(cfg.world_model.fragility.vpin_toxicity_weight)),
        cfg.world_model.fragility.vpin_toxicity_weight, "world_model.fragility.vpin_toxicity_weight");

    cfg.world_model.hysteresis.enabled = parse_bool(
        get_tracked( "world_model.hysteresis.enabled", cfg.world_model.hysteresis.enabled ? "true" : "false"),
        cfg.world_model.hysteresis.enabled, "world_model.hysteresis.enabled");
    cfg.world_model.hysteresis.confirmation_ticks = static_cast<int>(parse_double(
        get_tracked( "world_model.hysteresis.confirmation_ticks", std::to_string(cfg.world_model.hysteresis.confirmation_ticks)),
        static_cast<double>(cfg.world_model.hysteresis.confirmation_ticks), "world_model.hysteresis.confirmation_ticks"));
    cfg.world_model.hysteresis.min_dwell_ticks = static_cast<int>(parse_double(
        get_tracked( "world_model.hysteresis.min_dwell_ticks", std::to_string(cfg.world_model.hysteresis.min_dwell_ticks)),
        static_cast<double>(cfg.world_model.hysteresis.min_dwell_ticks), "world_model.hysteresis.min_dwell_ticks"));

    // ── world_model: feedback, history, persistence, suitability ──────────
    cfg.world_model.feedback.enabled = parse_bool(
        get_tracked( "world_model.feedback.enabled", cfg.world_model.feedback.enabled ? "true" : "false"),
        cfg.world_model.feedback.enabled, "world_model.feedback.enabled");
    cfg.world_model.feedback.ema_alpha = parse_double(
        get_tracked( "world_model.feedback.ema_alpha", std::to_string(cfg.world_model.feedback.ema_alpha)),
        cfg.world_model.feedback.ema_alpha, "world_model.feedback.ema_alpha");

    cfg.world_model.history.max_entries = static_cast<size_t>(parse_double(
        get_tracked( "world_model.history.max_entries", std::to_string(cfg.world_model.history.max_entries)),
        static_cast<double>(cfg.world_model.history.max_entries), "world_model.history.max_entries"));
    cfg.world_model.history.tendency_lookback = static_cast<size_t>(parse_double(
        get_tracked( "world_model.history.tendency_lookback", std::to_string(cfg.world_model.history.tendency_lookback)),
        static_cast<double>(cfg.world_model.history.tendency_lookback), "world_model.history.tendency_lookback"));

    cfg.world_model.persistence.history_blend_weight = parse_double(
        get_tracked( "world_model.persistence.history_blend_weight", std::to_string(cfg.world_model.persistence.history_blend_weight)),
        cfg.world_model.persistence.history_blend_weight, "world_model.persistence.history_blend_weight");
    cfg.world_model.persistence.min_history_for_empirical = static_cast<size_t>(parse_double(
        get_tracked( "world_model.persistence.min_history_for_empirical", std::to_string(cfg.world_model.persistence.min_history_for_empirical)),
        static_cast<double>(cfg.world_model.persistence.min_history_for_empirical), "world_model.persistence.min_history_for_empirical"));

    cfg.world_model.suitability.feedback_blend_weight = parse_double(
        get_tracked( "world_model.suitability.feedback_blend_weight", std::to_string(cfg.world_model.suitability.feedback_blend_weight)),
        cfg.world_model.suitability.feedback_blend_weight, "world_model.suitability.feedback_blend_weight");
    cfg.world_model.suitability.hard_veto_threshold = parse_double(
        get_tracked( "world_model.suitability.hard_veto_threshold", std::to_string(cfg.world_model.suitability.hard_veto_threshold)),
        cfg.world_model.suitability.hard_veto_threshold, "world_model.suitability.hard_veto_threshold");
    cfg.world_model.suitability.min_trades_for_feedback = static_cast<size_t>(parse_double(
        get_tracked( "world_model.suitability.min_trades_for_feedback", std::to_string(cfg.world_model.suitability.min_trades_for_feedback)),
        static_cast<double>(cfg.world_model.suitability.min_trades_for_feedback), "world_model.suitability.min_trades_for_feedback"));

    // ── Секция futures ───────────────────────────────────────────────────
    {
        auto& f = cfg.futures;
        auto futures_enabled_str = get_tracked( "futures.enabled", "true");
        f.enabled = parse_bool(futures_enabled_str, true, "futures.enabled");

        f.product_type = get_tracked( "futures.product_type", f.product_type);
        f.margin_coin  = get_tracked( "futures.margin_coin", f.margin_coin);
        f.margin_mode  = get_tracked( "futures.margin_mode", f.margin_mode);

        f.default_leverage = static_cast<int>(parse_double(
            get_tracked( "futures.default_leverage", std::to_string(f.default_leverage)),
            static_cast<double>(f.default_leverage), "futures.default_leverage"));
        f.max_leverage = static_cast<int>(parse_double(
            get_tracked( "futures.max_leverage", std::to_string(f.max_leverage)),
            static_cast<double>(f.max_leverage), "futures.max_leverage"));
        f.min_leverage = static_cast<int>(parse_double(
            get_tracked( "futures.min_leverage", std::to_string(f.min_leverage)),
            static_cast<double>(f.min_leverage), "futures.min_leverage"));

        // Leverage по режиму рынка
        f.leverage_trending = static_cast<int>(parse_double(
            get_tracked( "futures.leverage_trending", std::to_string(f.leverage_trending)),
            static_cast<double>(f.leverage_trending), "futures.leverage_trending"));
        f.leverage_ranging = static_cast<int>(parse_double(
            get_tracked( "futures.leverage_ranging", std::to_string(f.leverage_ranging)),
            static_cast<double>(f.leverage_ranging), "futures.leverage_ranging"));
        f.leverage_volatile = static_cast<int>(parse_double(
            get_tracked( "futures.leverage_volatile", std::to_string(f.leverage_volatile)),
            static_cast<double>(f.leverage_volatile), "futures.leverage_volatile"));

        // Защита от ликвидации
        f.liquidation_buffer_pct = parse_double(
            get_tracked( "futures.liquidation_buffer_pct", std::to_string(f.liquidation_buffer_pct)),
            f.liquidation_buffer_pct, "futures.liquidation_buffer_pct");

        // Фандинг
        f.funding_rate_threshold = parse_double(
            get_tracked( "futures.funding_rate_threshold", std::to_string(f.funding_rate_threshold)),
            f.funding_rate_threshold, "futures.funding_rate_threshold");
        f.funding_rate_penalty = parse_double(
            get_tracked( "futures.funding_rate_penalty", std::to_string(f.funding_rate_penalty)),
            f.funding_rate_penalty, "futures.funding_rate_penalty");

        // Maintenance margin rate
        f.maintenance_margin_rate = parse_double(
            get_tracked( "futures.maintenance_margin_rate", std::to_string(f.maintenance_margin_rate)),
            f.maintenance_margin_rate, "futures.maintenance_margin_rate");

        // LeverageEngineConfig
        auto& le = f.leverage_engine;
        le.vol_low_atr = parse_double(
            get_tracked( "futures.leverage_engine.vol_low_atr", std::to_string(le.vol_low_atr)),
            le.vol_low_atr, "futures.leverage_engine.vol_low_atr");
        le.vol_mid_atr = parse_double(
            get_tracked( "futures.leverage_engine.vol_mid_atr", std::to_string(le.vol_mid_atr)),
            le.vol_mid_atr, "futures.leverage_engine.vol_mid_atr");
        le.vol_high_atr = parse_double(
            get_tracked( "futures.leverage_engine.vol_high_atr", std::to_string(le.vol_high_atr)),
            le.vol_high_atr, "futures.leverage_engine.vol_high_atr");
        le.vol_extreme_atr = parse_double(
            get_tracked( "futures.leverage_engine.vol_extreme_atr", std::to_string(le.vol_extreme_atr)),
            le.vol_extreme_atr, "futures.leverage_engine.vol_extreme_atr");
        le.vol_floor = parse_double(
            get_tracked( "futures.leverage_engine.vol_floor", std::to_string(le.vol_floor)),
            le.vol_floor, "futures.leverage_engine.vol_floor");
        le.conviction_breakpoint = parse_double(
            get_tracked( "futures.leverage_engine.conviction_breakpoint", std::to_string(le.conviction_breakpoint)),
            le.conviction_breakpoint, "futures.leverage_engine.conviction_breakpoint");
        le.conviction_min_mult = parse_double(
            get_tracked( "futures.leverage_engine.conviction_min_mult", std::to_string(le.conviction_min_mult)),
            le.conviction_min_mult, "futures.leverage_engine.conviction_min_mult");
        le.conviction_max_mult = parse_double(
            get_tracked( "futures.leverage_engine.conviction_max_mult", std::to_string(le.conviction_max_mult)),
            le.conviction_max_mult, "futures.leverage_engine.conviction_max_mult");
        le.drawdown_floor_mult = parse_double(
            get_tracked( "futures.leverage_engine.drawdown_floor_mult", std::to_string(le.drawdown_floor_mult)),
            le.drawdown_floor_mult, "futures.leverage_engine.drawdown_floor_mult");
        le.drawdown_halfpoint_pct = parse_double(
            get_tracked( "futures.leverage_engine.drawdown_halfpoint_pct", std::to_string(le.drawdown_halfpoint_pct)),
            le.drawdown_halfpoint_pct, "futures.leverage_engine.drawdown_halfpoint_pct");
        le.ema_alpha = parse_double(
            get_tracked( "futures.leverage_engine.ema_alpha", std::to_string(le.ema_alpha)),
            le.ema_alpha, "futures.leverage_engine.ema_alpha");
        le.taker_fee_rate = parse_double(
            get_tracked( "futures.leverage_engine.taker_fee_rate", std::to_string(le.taker_fee_rate)),
            le.taker_fee_rate, "futures.leverage_engine.taker_fee_rate");
        le.min_leverage_change_delta = static_cast<int>(parse_double(
            get_tracked( "futures.leverage_engine.min_leverage_change_delta", std::to_string(le.min_leverage_change_delta)),
            static_cast<double>(le.min_leverage_change_delta), "futures.leverage_engine.min_leverage_change_delta"));
    }

    // ── Секция uncertainty ───────────────────────────────────────────────
    {
        auto& u = cfg.uncertainty;
        u.w_regime = parse_double(
            get_tracked( "uncertainty.w_regime", std::to_string(u.w_regime)),
            u.w_regime, "uncertainty.w_regime");
        u.w_signal = parse_double(
            get_tracked( "uncertainty.w_signal", std::to_string(u.w_signal)),
            u.w_signal, "uncertainty.w_signal");
        u.w_data_quality = parse_double(
            get_tracked( "uncertainty.w_data_quality", std::to_string(u.w_data_quality)),
            u.w_data_quality, "uncertainty.w_data_quality");
        u.w_execution = parse_double(
            get_tracked( "uncertainty.w_execution", std::to_string(u.w_execution)),
            u.w_execution, "uncertainty.w_execution");
        u.w_portfolio = parse_double(
            get_tracked( "uncertainty.w_portfolio", std::to_string(u.w_portfolio)),
            u.w_portfolio, "uncertainty.w_portfolio");
        u.w_ml = parse_double(
            get_tracked( "uncertainty.w_ml", std::to_string(u.w_ml)),
            u.w_ml, "uncertainty.w_ml");
        u.w_correlation = parse_double(
            get_tracked( "uncertainty.w_correlation", std::to_string(u.w_correlation)),
            u.w_correlation, "uncertainty.w_correlation");
        u.w_transition = parse_double(
            get_tracked( "uncertainty.w_transition", std::to_string(u.w_transition)),
            u.w_transition, "uncertainty.w_transition");
        u.w_operational = parse_double(
            get_tracked( "uncertainty.w_operational", std::to_string(u.w_operational)),
            u.w_operational, "uncertainty.w_operational");

        u.threshold_low = parse_double(
            get_tracked( "uncertainty.threshold_low", std::to_string(u.threshold_low)),
            u.threshold_low, "uncertainty.threshold_low");
        u.threshold_moderate = parse_double(
            get_tracked( "uncertainty.threshold_moderate", std::to_string(u.threshold_moderate)),
            u.threshold_moderate, "uncertainty.threshold_moderate");
        u.threshold_high = parse_double(
            get_tracked( "uncertainty.threshold_high", std::to_string(u.threshold_high)),
            u.threshold_high, "uncertainty.threshold_high");

        u.hysteresis_up = parse_double(
            get_tracked( "uncertainty.hysteresis_up", std::to_string(u.hysteresis_up)),
            u.hysteresis_up, "uncertainty.hysteresis_up");
        u.hysteresis_down = parse_double(
            get_tracked( "uncertainty.hysteresis_down", std::to_string(u.hysteresis_down)),
            u.hysteresis_down, "uncertainty.hysteresis_down");

        u.ema_alpha = parse_double(
            get_tracked( "uncertainty.ema_alpha", std::to_string(u.ema_alpha)),
            u.ema_alpha, "uncertainty.ema_alpha");
        u.size_floor = parse_double(
            get_tracked( "uncertainty.size_floor", std::to_string(u.size_floor)),
            u.size_floor, "uncertainty.size_floor");
        u.threshold_ceiling = parse_double(
            get_tracked( "uncertainty.threshold_ceiling", std::to_string(u.threshold_ceiling)),
            u.threshold_ceiling, "uncertainty.threshold_ceiling");

        u.consecutive_extreme_for_cooldown = static_cast<int>(parse_double(
            get_tracked( "uncertainty.consecutive_extreme_for_cooldown", std::to_string(u.consecutive_extreme_for_cooldown)),
            static_cast<double>(u.consecutive_extreme_for_cooldown), "uncertainty.consecutive_extreme_for_cooldown"));
        u.cooldown_duration_ns = static_cast<int64_t>(parse_double(
            get_tracked( "uncertainty.cooldown_duration_s", "60"),
            60.0, "uncertainty.cooldown_duration_s") * 1'000'000'000.0);
        u.consecutive_high_for_defensive = static_cast<int>(parse_double(
            get_tracked( "uncertainty.consecutive_high_for_defensive", std::to_string(u.consecutive_high_for_defensive)),
            static_cast<double>(u.consecutive_high_for_defensive), "uncertainty.consecutive_high_for_defensive"));

        u.liquidity_ratio_penalty_threshold = parse_double(
            get_tracked( "uncertainty.liquidity_ratio_penalty_threshold", std::to_string(u.liquidity_ratio_penalty_threshold)),
            u.liquidity_ratio_penalty_threshold, "uncertainty.liquidity_ratio_penalty_threshold");
    }

    // Fail-fast: если при парсинге обнаружены невалидные значения — не запускаться
    if (!parse_errors.empty()) {
        std::cerr << "[CONFIG] ОШИБКИ ПАРСИНГА КОНФИГУРАЦИИ (" << parse_errors.size() << "):\n";
        for (const auto& e : parse_errors) {
            std::cerr << "  - " << e << "\n";
        }
        return Err<AppConfig>(TbError::ConfigLoadFailed);
    }

    // N-6: Detect unknown/mistyped config keys
    {
        std::vector<std::string> unknown_keys;
        for (const auto& [key, _] : kv) {
            if (!consumed_keys.contains(key)) {
                unknown_keys.push_back(key);
            }
        }
        if (!unknown_keys.empty()) {
            std::sort(unknown_keys.begin(), unknown_keys.end());
            std::cerr << "[CONFIG] WARNING: " << unknown_keys.size()
                      << " unknown config key(s) detected (possible typos):\n";
            for (const auto& k : unknown_keys) {
                std::cerr << "  - " << k << "\n";
            }
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
