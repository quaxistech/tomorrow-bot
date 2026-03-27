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
#include "ai/ai_advisory_types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::config {

using ai::AIAdvisoryConfig;

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

    // --- v4: Percentile scoring ---
    int percentile_window_size{500};
    double percentile_severity_threshold{0.95};

    // --- v4: Correlation matrix ---
    double correlation_alpha{0.02};
    double correlation_breakdown_threshold{0.4};

    // --- v4: Time-weighted EMA & Multi-timeframe ---
    double baseline_halflife_fast_ms{30'000.0};
    double baseline_halflife_medium_ms{300'000.0};
    double baseline_halflife_slow_ms{1'800'000.0};
    double timeframe_divergence_threshold{2.5};

    // --- v4: Hysteresis ---
    double hysteresis_enter_severity{0.5};
    double hysteresis_exit_severity{0.25};
    double hysteresis_confidence_penalty{0.15};

    // --- v4: Event sourcing ---
    int64_t audit_log_max_size{10'000};
};

/// Настройки движка принятия решений (conviction, конфликт-разрешение, advanced features)
struct DecisionConfig {
    double min_conviction_threshold{0.28};      ///< Мин. conviction для одобрения сделки
    double conflict_dominance_threshold{0.60};  ///< Мин. доминирование одного направления (BUY/SELL) при конфликте

    // === Advanced features (professional-grade) ===
    bool enable_regime_threshold_scaling{true};   ///< Адаптивный порог по режиму рынка
    bool enable_regime_dominance_scaling{true};   ///< Адаптивный порог доминирования по режиму
    bool enable_time_decay{true};                 ///< Time decay для stale-сигналов
    double time_decay_halflife_ms{700.0};         ///< Период полураспада conviction (мс)
    bool enable_ensemble_conviction{true};        ///< Ансамблевый бонус при согласии стратегий
    double ensemble_agreement_bonus{0.08};        ///< Бонус за каждую согласную стратегию
    double ensemble_max_bonus{0.20};              ///< Макс. бонус от ансамбля
    bool enable_portfolio_awareness{true};        ///< Учёт просадки/серии убытков в пороге
    double drawdown_boost_scale{0.10};            ///< +10% к порогу за каждые 5% просадки
    bool enable_execution_cost_modeling{true};     ///< Пенальти conviction за spread/slippage
    double max_acceptable_cost_bps{80.0};          ///< Вето если execution cost > N bps
    bool enable_time_skew_detection{true};        ///< Детекция рассинхронизации состояний
};

/// Настройки управления позицией (стоп-лосс, тейк-профит, тайминг)
struct TradingParamsConfig {
    double atr_stop_multiplier{2.0};            ///< Множитель ATR для trailing stop (Chandelier Exit)
    double max_loss_per_trade_pct{1.0};         ///< Макс. убыток на сделку (% от капитала)
    double breakeven_atr_threshold{1.5};        ///< ATR-профит для переноса стопа в breakeven
    double partial_tp_atr_threshold{2.0};       ///< ATR-профит для частичного тейк-профита
    double partial_tp_fraction{0.5};            ///< Доля позиции для частичного TP (0.5 = 50%)
    int max_hold_loss_minutes{15};              ///< Макс. удержание убыточной позиции (минуты)
    int max_hold_absolute_minutes{60};          ///< Макс. удержание любой позиции (минуты)
    int order_cooldown_seconds{10};             ///< Кулдаун между ордерами (секунды)
    int stop_loss_cooldown_seconds{300};        ///< Кулдаун после стоп-лосса (секунды)
};

/// Настройки модуля исполнительной альфы
struct ExecutionAlphaConfig {
    // ── Базовые пороги ──
    double max_spread_bps_passive{15.0};      ///< Макс спред для пассивного исполнения [bps]
    double max_spread_bps_any{50.0};           ///< Макс спред для любого исполнения [bps]
    double adverse_selection_threshold{0.7};   ///< Порог токсичности для NoExecution [0..1]
    double urgency_passive_threshold{0.5};     ///< Срочность ниже → Passive
    double urgency_aggressive_threshold{0.8};  ///< Срочность выше → Aggressive
    double large_order_slice_threshold{0.1};   ///< Доля от глубины стакана → нарезка [0..1]

    // ── VPIN ──
    double vpin_toxic_threshold{0.65};   ///< VPIN выше → высокая токсичность
    double vpin_weight{0.40};            ///< Вес VPIN в расчёте adverse selection

    // ── Дисбаланс стакана ──
    double imbalance_favorable_threshold{0.30};   ///< Благоприятный → Passive предпочтителен
    double imbalance_unfavorable_threshold{0.30};  ///< Неблагоприятный → предпочесть Hybrid

    // ── Ценообразование ──
    bool   use_weighted_mid_price{true};    ///< Использовать weighted_mid_price
    double limit_price_passive_bps{3.0};    ///< Улучшение к best bid/ask для maker [bps]

    // ── Срочность ──
    double urgency_cusum_boost{0.15};   ///< Прирост при сигнале CUSUM
    double urgency_tod_weight{0.10};    ///< Вес time-of-day alpha score

    // ── Вероятность заполнения ──
    double min_fill_probability_passive{0.25};  ///< Нижняя граница для Passive/PostOnly

    // ── PostOnly условия ──
    double postonly_spread_threshold_bps{4.5};  ///< Спред ниже → PostOnly кандидат [bps]
    double postonly_urgency_max{0.35};           ///< Срочность ниже → PostOnly кандидат
    double postonly_adverse_max{0.35};           ///< Токсичность ниже → PostOnly кандидат
};

/// Полная конфигурация приложения
struct AppConfig {
    ExchangeConfig       exchange;         ///< Настройки биржи
    LoggingConfig        logging;          ///< Настройки логирования
    MetricsConfig        metrics;          ///< Настройки метрик
    HealthConfig         health;           ///< Настройки проверки здоровья
    RiskConfig           risk;             ///< Настройки риск-менеджера
    TradingModeConfig    trading;          ///< Настройки режима торговли
    PairSelectionConfig  pair_selection;   ///< Настройки выбора торговых пар
    AdversarialDefenseConfig adversarial_defense; ///< Защита от враждебных market conditions
    AIAdvisoryConfig     ai_advisory;      ///< AI Advisory — правиловый/ML анализ
    DecisionConfig       decision;         ///< Настройки движка принятия решений
    TradingParamsConfig  trading_params;   ///< Настройки управления позицией
    ExecutionAlphaConfig execution_alpha;  ///< Настройки модуля исполнительной альфы
    std::string          config_hash;      ///< SHA-256 хеш файла конфигурации (для аудита)
};

} // namespace tb::config
