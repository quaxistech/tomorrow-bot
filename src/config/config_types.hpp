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
#include "regime/regime_config.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::config {

using regime::RegimeConfig;

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

/// Настройки риск-менеджера
struct RiskConfig {
    double max_position_notional{10000.0};  ///< Максимальный номинальный объём позиции (USD)
    double max_daily_loss_pct{2.0};         ///< Максимальный дневной убыток (% от капитала)
    double max_drawdown_pct{5.0};           ///< Максимальная просадка (% от капитала)
    bool   kill_switch_enabled{true};       ///< Включён ли аварийный выключатель

    // === Расширенные параметры риска (desk-grade) ===
    double max_strategy_daily_loss_pct{1.5};     ///< Макс дневной убыток одной стратегии (% капитала)
    double max_strategy_exposure_pct{30.0};      ///< Макс экспозиция одной стратегии (% капитала)
    double max_symbol_concentration_pct{35.0};   ///< Макс доля капитала на один символ (%)
    int    max_same_direction_positions{3};       ///< Макс позиций в одном направлении
    bool   regime_aware_limits_enabled{true};     ///< Включить адаптацию лимитов по режиму
    double stress_regime_scale{0.5};              ///< Множитель лимитов в стрессовых режимах
    double trending_regime_scale{1.2};            ///< Множитель лимитов в трендовых режимах
    double chop_regime_scale{0.7};                ///< Множитель лимитов в боковых режимах
    int    max_trades_per_hour{8};                ///< Макс закрытых сделок в час
    double min_trade_interval_sec{30.0};          ///< Мин интервал между сделками одного символа (секунды)
    double max_adverse_excursion_pct{3.0};        ///< Макс неблагоприятное отклонение (% капитала)
    double max_realized_daily_loss_pct{1.5};      ///< Макс реализованный дневной убыток (%)
    int    utc_cutoff_hour{-1};                   ///< Час UTC прекращения торговли (-1 = отключено)
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

/// Параметризованные веса скоринга (замена magic numbers)
struct ScorerConfig {
    /// Версия алгоритма скоринга
    std::string version{"v4"};

    // --- Веса компонентов (сумма = 100) ---
    double momentum_max{40.0};
    double trend_max{25.0};
    double tradability_max{25.0};
    double quality_max{10.0};

    // --- Momentum параметры ---
    double momentum_log_multiplier{14.5};
    double acceleration_log_multiplier{14.0};
    double fresh_start_multiplier{2.5};
    double fresh_start_roc_24h_cap{10.0};
    double fresh_start_roc_4h_min{0.5};

    // --- Trend параметры ---
    double adx_weak_threshold{15.0};
    double adx_moderate_threshold{25.0};
    double adx_strong_threshold{50.0};
    double bullish_ratio_min{0.50};
    double roc_normalization_factor{5.0};

    // --- Tradability параметры ---
    double volume_tier_excellent{5'000'000.0};
    double volume_tier_good{1'000'000.0};
    double volume_tier_acceptable{500'000.0};
    double volume_tier_minimal{100'000.0};
    double spread_decay_constant{15.0};
    double volatility_low_threshold{0.5};
    double volatility_high_threshold{20.0};

    // --- Hard filters ---
    double filter_min_change_24h{-1.0};
    double filter_max_change_24h{20.0};
    double filter_exhausted_pump_24h{10.0};
    double filter_exhausted_pump_ratio{0.25};
    double stagnation_threshold{1.0};
    double stagnation_penalty{0.3};
    double steady_gainer_min{2.0};
    double steady_gainer_max{10.0};
    double steady_gainer_bonus{8.0};
    double negative_change_penalty{0.5};

    // --- Futures bidirectional mode ---
    bool futures_mode{false};                    ///< true = score absolute trend strength (both long & short)
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

    // --- Тип рынка (spot / futures) ---
    bool futures_enabled{false};                 ///< true = сканировать USDT-M Futures пары
    std::string product_type{"USDT-FUTURES"};    ///< Тип продукта для фьючерсов

    // --- Расширенные настройки сканера (professional-grade) ---
    int max_candidates_for_candles{30};          ///< Макс. кандидатов для загрузки свечей
    int candle_fetch_concurrency{5};             ///< Параллельных загрузок свечей
    int candle_history_hours{48};                ///< Глубина истории свечей
    int scan_timeout_ms{60'000};                 ///< Таймаут всего сканирования (мс)
    int api_retry_max{3};                        ///< Макс. повторных попыток API
    int api_retry_base_delay_ms{200};            ///< Базовая задержка retry (мс)
    int circuit_breaker_threshold{5};            ///< Порог ошибок для circuit breaker
    int circuit_breaker_reset_ms{300'000};       ///< Время сброса circuit breaker (мс)
    double max_correlation_in_basket{0.85};      ///< Макс. корреляция между парами в корзине
    int max_pairs_per_sector{2};                 ///< Макс. пар из одного сектора
    double min_liquidity_depth_usdt{50'000.0};   ///< Мин. глубина ликвидности
    bool enable_diversification{true};           ///< Включить диверсификацию корзины
    bool persist_scan_results{true};             ///< Сохранять результаты в persistence
    ScorerConfig scorer;                         ///< Конфигурация scorer-а (вложенный)
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
    double drawdown_max_boost{0.25};               ///< Макс. повышение порога от просадки
    double consecutive_loss_boost{0.03};            ///< +3% к порогу за каждую убыточную серию
    bool enable_execution_cost_modeling{true};     ///< Пенальти conviction за spread/slippage
    double max_acceptable_cost_bps{80.0};          ///< Вето если execution cost > N bps
    bool enable_time_skew_detection{true};        ///< Детекция рассинхронизации состояний
};

/// Настройки управления позицией (стоп-лосс, тейк-профит, тайминг)
struct TradingParamsConfig {
    double atr_stop_multiplier{2.0};            ///< Множитель ATR для trailing stop (Chandelier Exit)
    double max_loss_per_trade_pct{1.0};         ///< Макс. убыток на сделку (% от капитала)
    double price_stop_loss_pct{3.0};            ///< Макс. убыток на сделку (% от цены входа, спот)
    double min_risk_reward_ratio{1.5};          ///< Минимальный R:R для открытия позиции
    double breakeven_atr_threshold{1.5};        ///< ATR-профит для переноса стопа в breakeven
    double partial_tp_atr_threshold{2.0};       ///< ATR-профит для частичного тейк-профита
    double partial_tp_fraction{0.5};            ///< Доля позиции для частичного TP (0.5 = 50%)
    int max_hold_loss_minutes{15};              ///< Макс. удержание убыточной позиции (минуты)
    int max_hold_absolute_minutes{60};          ///< Макс. удержание любой позиции (минуты)
    int min_hold_minutes{30};                   ///< Мин. удержание позиции до стратегического закрытия (минуты)
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

/// Настройки модуля opportunity cost
struct OpportunityCostConfig {
    // ── Пороги net edge (базисные пункты) ──
    double min_net_expected_bps{0.0};          ///< Мин чистый ожидаемый доход для входа
    double execute_min_net_bps{15.0};          ///< Мин чистый доход для немедленного исполнения

    // ── Пороги экспозиции ──
    double high_exposure_threshold{0.75};      ///< Порог высокой экспозиции [0,1]
    double high_exposure_min_conviction{0.65}; ///< Мин conviction при высокой экспозиции

    // ── Пороги концентрации ──
    double max_symbol_concentration{0.25};     ///< Макс доля капитала на один символ
    double max_strategy_concentration{0.35};   ///< Макс доля капитала на одну стратегию

    // ── Пороги капитала ──
    double capital_exhaustion_threshold{0.90}; ///< Порог исчерпания капитала [0,1]

    // ── Веса скоринга ──
    double weight_conviction{0.35};            ///< Вес conviction в composite score
    double weight_net_edge{0.35};              ///< Вес net edge
    double weight_capital_efficiency{0.15};    ///< Вес capital efficiency
    double weight_urgency{0.15};               ///< Вес urgency

    // ── Масштабирование expected return ──
    double conviction_to_bps_scale{120.0};     ///< Масштаб: conviction 1.0 → N bps

    // ── Upgrade ──
    double upgrade_min_edge_advantage_bps{10.0}; ///< Мин разница edge для Upgrade vs худшей позиции

    // ── Drawdown penalty ──
    double drawdown_penalty_scale{0.5};        ///< Множитель: +X к порогу за каждые 5% просадки
};

// ─── Фьючерсная торговля ──────────────────────────────────────────────────────

/// Конфигурация фьючерсной торговли (USDT-M)
struct FuturesConfig {
    bool enabled{false};                       ///< Включить фьючерсный режим
    std::string product_type{"USDT-FUTURES"};  ///< Тип продукта Bitget
    std::string margin_coin{"USDT"};           ///< Маржинальная монета
    std::string margin_mode{"isolated"};       ///< Режим маржи: isolated / crossed

    // Кредитное плечо
    int default_leverage{5};                   ///< Базовое плечо по умолчанию
    int max_leverage{20};                      ///< Максимально допустимое плечо
    int min_leverage{1};                       ///< Минимальное допустимое плечо (floor)

    // Адаптивное плечо по режиму рынка
    int leverage_trending{10};                 ///< Плечо в тренде
    int leverage_ranging{5};                   ///< Плечо в боковике
    int leverage_volatile{3};                  ///< Плечо при высокой волатильности
    int leverage_stress{1};                    ///< Плечо при стрессе

    // Защита от ликвидации
    double liquidation_buffer_pct{5.0};        ///< Мин. буфер до ликвидации (%)
    double max_leverage_drawdown_scale{0.5};   ///< Снижение плеча: ×0.5 за каждые 10% drawdown

    // Фандинг
    double funding_rate_threshold{0.03};       ///< Порог фандинга для предупреждения (3%)
    double funding_rate_penalty{0.5};          ///< Пенальти к плечу при высоком фандинге

    // Maintenance margin (Bitget default for small positions)
    double maintenance_margin_rate{0.004};     ///< Maintenance margin rate (0.4%)
};

/// Полная конфигурация приложения
struct AppConfig {
    ExchangeConfig       exchange;         ///< Настройки биржи
    LoggingConfig        logging;          ///< Настройки логирования
    MetricsConfig        metrics;          ///< Настройки метрик
    RiskConfig           risk;             ///< Настройки риск-менеджера
    TradingModeConfig    trading;          ///< Настройки режима торговли
    PairSelectionConfig  pair_selection;   ///< Настройки выбора торговых пар
    DecisionConfig       decision;         ///< Настройки движка принятия решений
    TradingParamsConfig  trading_params;   ///< Настройки управления позицией
    ExecutionAlphaConfig execution_alpha;  ///< Настройки модуля исполнительной альфы
    OpportunityCostConfig opportunity_cost; ///< Настройки модуля opportunity cost
    RegimeConfig         regime;           ///< Настройки классификатора рыночных режимов
    FuturesConfig        futures;          ///< Настройки фьючерсной торговли
    std::string          config_hash;      ///< SHA-256 хеш файла конфигурации (для аудита)
};

} // namespace tb::config
