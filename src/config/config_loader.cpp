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
