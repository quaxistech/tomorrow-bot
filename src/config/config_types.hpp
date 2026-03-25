/**
 * @file config_types.hpp
 * @brief Типизированные структуры конфигурации системы Tomorrow Bot
 * 
 * Все настройки системы представлены строго типизированными структурами.
 * Секреты НЕ хранятся в конфигурации — только ссылки на имена
 * переменных окружения (api_key_ref, api_secret_ref).
 */
#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::config {

/// Настройки подключения к бирже
struct ExchangeConfig {
    std::string endpoint_rest;      ///< URL REST API биржи
    std::string endpoint_ws;        ///< URL WebSocket API биржи
    std::string api_key_ref;        ///< Имя переменной окружения с API ключом (НЕ сам ключ!)
    std::string api_secret_ref;     ///< Имя переменной окружения с API секретом
    std::string passphrase_ref;     ///< Имя переменной окружения с пассфразой
    int         timeout_ms{5000};   ///< Таймаут HTTP запросов в миллисекундах
};

/// Настройки логирования
struct LoggingConfig {
    std::string level{"info"};          ///< Уровень логирования: trace/debug/info/warn/error/critical
    std::string format{"text"};         ///< Формат: text или json
    std::string output_path{"-"};       ///< Путь к файлу лога; "-" означает stdout
    bool        structured_json{false}; ///< Использовать структурированный JSON формат
};

/// Настройки экспорта метрик (Prometheus)
struct MetricsConfig {
    bool        enabled{true};          ///< Включён ли сервер метрик
    int         port{9090};             ///< Порт HTTP сервера метрик
    std::string path{"/metrics"};       ///< Путь для scrape эндпоинта
};

/// Настройки сервиса проверки здоровья
struct HealthConfig {
    bool enabled{true};                 ///< Включён ли HTTP сервер здоровья
    int  port{8080};                    ///< Порт HTTP сервера
};

/// Настройки риск-менеджера
struct RiskConfig {
    double max_position_notional{10000.0};  ///< Максимальный номинальный объём позиции (USD)
    double max_daily_loss_pct{2.0};         ///< Максимальный дневной убыток (% от капитала)
    double max_drawdown_pct{5.0};           ///< Максимальная просадка (% от капитала)
    bool   kill_switch_enabled{true};       ///< Включён ли аварийный выключатель
};

/// Настройки режима торговли
struct TradingModeConfig {
    TradingMode mode{TradingMode::Paper};   ///< Текущий режим торговли
    double initial_capital{10000.0};         ///< Начальный капитал на аккаунте (USD)
};

/// Режим выбора торговых пар
enum class PairSelectionMode {
    Auto,     ///< Автоматический сканирование + выбор лучших по скорингу
    Manual    ///< Фиксированный список символов из конфига
};

/// Настройки системы выбора торговых пар
struct PairSelectionConfig {
    PairSelectionMode mode{PairSelectionMode::Auto};
    int top_n{5};                                ///< Сколько лучших пар выбирать
    double min_volume_usdt{500'000.0};           ///< Мин. 24ч объём USDT для прохождения фильтра
    double max_spread_bps{50.0};                 ///< Макс. допустимый спред (базисных пунктов)
    int rotation_interval_hours{24};             ///< Интервал ротации (в часах)
    std::vector<std::string> manual_symbols;     ///< Символы для ручного режима
    std::vector<std::string> blacklist;          ///< Символы, запрещённые для торговли
};

/// Runtime-настройки защиты от враждебных рыночных условий
struct AdversarialDefenseConfig {
    bool enabled{true};
    bool fail_closed_on_invalid_data{true};
    bool auto_cooldown_on_veto{true};
    double auto_cooldown_severity{0.85};
    double spread_explosion_threshold_bps{100.0};
    double spread_normal_bps{20.0};
    double min_liquidity_depth{50.0};
    double book_imbalance_threshold{0.8};
    double book_instability_threshold{0.7};
    double toxic_flow_ratio_threshold{1.8};
    double aggressive_flow_threshold{0.8};
    double vpin_toxic_threshold{0.7};
    int64_t cooldown_duration_ms{30000};
    int64_t post_shock_cooldown_ms{60000};
    int64_t max_market_data_age_ns{2'000'000'000LL};
    double max_confidence_reduction{0.8};
    double max_threshold_expansion{2.0};

    // --- Compound threat & recovery ---
    double compound_threat_factor{0.5};
    double cooldown_severity_scale{1.5};
    int64_t recovery_duration_ms{10000};
    double recovery_confidence_floor{0.6};

    // --- Spread velocity ---
    double spread_velocity_threshold_bps_per_sec{50.0};

    // --- Adaptive baseline ---
    double baseline_alpha{0.01};
    int64_t baseline_warmup_ticks{200};
    double z_score_spread_threshold{3.0};
    double z_score_depth_threshold{3.0};
    double z_score_ratio_threshold{3.0};
    int64_t baseline_stale_reset_ms{300'000LL};

    // --- Threat memory ---
    double threat_memory_alpha{0.15};
    double threat_memory_residual_factor{0.3};
    int threat_escalation_ticks{5};
    double threat_escalation_boost{0.1};

    // --- Depth asymmetry ---
    double depth_asymmetry_threshold{0.3};

    // --- Cross-signal amplification ---
    double cross_signal_amplification{0.3};
};

/// Полная конфигурация приложения
struct AppConfig {
    ExchangeConfig       exchange;       ///< Настройки биржи
    LoggingConfig        logging;        ///< Настройки логирования
    MetricsConfig        metrics;        ///< Настройки метрик
    HealthConfig         health;         ///< Настройки проверки здоровья
    RiskConfig           risk;           ///< Настройки риск-менеджера
    TradingModeConfig    trading;        ///< Настройки режима торговли
    PairSelectionConfig  pair_selection; ///< Настройки выбора торговых пар
    AdversarialDefenseConfig adversarial_defense; ///< Защита от враждебных market conditions
    std::string          config_hash;    ///< SHA-256 хеш файла конфигурации (для аудита)
};

} // namespace tb::config
