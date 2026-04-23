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
#include "world_model/world_model_config.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::config {

using regime::RegimeConfig;
using world_model::WorldModelConfig;
using uncertainty::UncertaintyConfig;

/// Настройки подключения к бирже
struct ExchangeConfig {
    std::string endpoint_rest;          ///< URL REST API биржи
    std::string endpoint_ws;            ///< URL WebSocket API биржи (public)
    std::string endpoint_ws_private;    ///< URL WebSocket API биржи (private, authenticated)
    std::string api_key_ref;            ///< Имя переменной окружения с API ключом (НЕ сам ключ!)
    std::string api_secret_ref;         ///< Имя переменной окружения с API секретом
    std::string passphrase_ref;         ///< Имя переменной окружения с пассфразой
    int         timeout_ms{5000};       ///< Таймаут HTTP запросов в миллисекундах
    bool        use_private_ws{true};   ///< Использовать private WS для заказов/fills
};

/// Настройки логирования
struct LoggingConfig {
    std::string level{"info"};          ///< Уровень логирования: trace/debug/info/warn/error/critical
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
    double max_intraday_drawdown_pct{3.0};         ///< Макс внутридневная просадка (%)
    int    utc_cutoff_hour{-1};                   ///< Час UTC прекращения торговли (-1 = отключено)
};

/// Настройки режима торговли
struct TradingModeConfig {
    TradingMode mode{TradingMode::Production};   ///< Текущий режим торговли
    double initial_capital{10000.0};         ///< Начальный капитал на аккаунте (USD)
};

/// Режим выбора торговых пар
enum class PairSelectionMode {
    Auto,     ///< Автоматический сканирование + выбор лучших по скорингу
    Manual    ///< Фиксированный список символов из конфига
};

/// Параметризованные веса скоринга (замена magic numbers)
struct ScorerConfig {
    // --- Пороги фильтрации, реально используемые в runtime (ScannerConfig) ---
    double volume_tier_minimal{100'000.0};      ///< Мин. объём для допуска (перезаписывает min_volume_usdt)
    double volatility_low_threshold{0.5};       ///< Мин. реализованная vol % на 5m (мёртвый инструмент)
    double volatility_high_threshold{20.0};     ///< Макс. реализованная vol % на 5m (опасный шум)
    double filter_min_change_24h{-1.0};         ///< Мин. 24ч изменение % для прохождения фильтра
    double filter_max_change_24h{20.0};         ///< Макс. 24ч изменение % (фильтр экстремальных pump)
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

    // --- Расширенные настройки сканера (professional-grade) ---
    int max_candidates_for_candles{30};          ///< Макс. кандидатов для загрузки свечей
    int scan_timeout_ms{60'000};                 ///< Таймаут всего сканирования (мс)
    int api_retry_max{3};                        ///< Макс. повторных попыток API
    int api_retry_base_delay_ms{200};            ///< Базовая задержка retry (мс)
    int circuit_breaker_threshold{5};            ///< Порог ошибок для circuit breaker
    int circuit_breaker_reset_ms{300'000};       ///< Время сброса circuit breaker (мс)
    double max_correlation_in_basket{0.85};      ///< Макс. корреляция между парами в корзине
    int max_pairs_per_sector{2};                 ///< Макс. пар из одного сектора
    double min_liquidity_depth_usdt{50'000.0};   ///< Мин. глубина ликвидности
    bool enable_diversification{true};           ///< Включить диверсификацию корзины
    ScorerConfig scorer;                         ///< Конфигурация scorer-а (вложенный)
};

/// Настройки движка принятия решений (conviction, конфликт-разрешение, advanced features).
/// Все дефолты калиброваны для USDT-M фьючерсного скальпинга.
struct DecisionConfig {
    /// Мин. conviction для одобрения сделки.
    /// Aldridge (2013) «High-Frequency Trading»: сигнал-к-шуму ≥ 0.4–0.6 для прибыльного входа
    /// в леверидж-инструменты. 0.45 — минимум; production рекомендует ≥ 0.62.
    double min_conviction_threshold{0.45};
    double conflict_dominance_threshold{0.60};  ///< Мин. доминирование одного направления (BUY/SELL) при конфликте

    // === Advanced features (professional-grade) ===
    bool enable_regime_threshold_scaling{true};   ///< Адаптивный порог по режиму рынка
    bool enable_regime_dominance_scaling{true};   ///< Адаптивный порог доминирования по режиму
    bool enable_time_decay{true};                 ///< Time decay для stale-сигналов
    /// Hasbrouck (2007): информационное полувремя ордер-бука 100–1000 мс.
    double time_decay_halflife_ms{500.0};
    bool enable_ensemble_conviction{true};        ///< Ансамблевый бонус при согласии стратегий
    /// Breiman (2001): выигрыш ансамбля падает с ростом корреляции. 6% — консервативно.
    double ensemble_agreement_bonus{0.06};
    double ensemble_max_bonus{0.15};              ///< Макс. бонус от ансамбля
    bool enable_portfolio_awareness{true};        ///< Учёт просадки/серии убытков в пороге
    /// Thorp (2006) «Kelly Criterion»: пропорционально снижаем ставку при просадке.
    double drawdown_boost_scale{0.02};
    double drawdown_max_boost{0.08};               ///< Макс. повышение порога от просадки
    /// Aronson (2007): серии 5–8 убытков нормальны в прибыльных скальп-системах.
    double consecutive_loss_boost{0.005};
    bool enable_execution_cost_modeling{true};     ///< Пенальти conviction за spread/slippage
    /// Bitget taker ≈ 6 bps + spread 2–5 bps ≈ 10 bps нормы. 50 bps — запас.
    double max_acceptable_cost_bps{50.0};
    bool enable_time_skew_detection{true};        ///< Детекция рассинхронизации состояний
};

/// Настройки управления позицией (стоп-лосс, тейк-профит, тайминг)
/// Все значения по умолчанию рассчитаны для USDT-M фьючерсного скальпинга.
struct TradingParamsConfig {
    /// Множитель ATR для trailing stop (Chandelier Exit).
    /// Wilder (1978): 2–3× ATR; для крипто-фьючерсов 2.0 — баланс между шумом и защитой.
    double atr_stop_multiplier{2.0};
    /// Макс. убыток на сделку (% от капитала). Стандарт риск-менеджмента: ≤ 1–2%.
    double max_loss_per_trade_pct{1.0};
    /// Макс. допустимое движение цены против позиции (% от цены входа).
    /// Safety net для фьючерсов с плечом: при 20× даже 1.5% = 30% капитала.
    double price_stop_loss_pct{1.5};
    /// Минимальный Risk:Reward для входа (Kelly criterion break-even).
    /// При WR ≥ 60% break-even R:R = 0.67. Значение 0.8 даёт буфер ~20%.
    /// Для high-WR скальпинга production-конфиг может снижать до 0.1.
    double min_risk_reward_ratio{0.8};
    /// ATR-профит для переноса стопа в breakeven.
    /// Tharp: ≥ 1×ATR; для скальпинга быстрее — 0.8×ATR покрывает round-trip комиссии.
    double breakeven_atr_threshold{0.8};
    /// ATR-профит для тейк-профита. 2×ATR = стандартный target (2R).
    double partial_tp_atr_threshold{2.0};
    /// Доля позиции для TP (0.5 = 50%). При 1.0 = полное закрытие.
    double partial_tp_fraction{0.5};
    /// Мин. удержание позиции до стратегического закрытия (минуты).
    /// Operational guard: не закрывать только что открытую позицию на слабый сигнал (anti-whipsaw).
    /// NOTE: Removed from alpha-path. Only infra watchdog remains.
    /// int min_hold_minutes — REMOVED (time-based exits prohibited)
    /// Кулдаун между ордерами (секунды). 10–15 с предотвращает спам API.
    int order_cooldown_seconds{10};
    /// Кулдаун после стоп-лосса (секунды). 3 мин — не входить в тот же чопающий рынок.
    int stop_loss_cooldown_seconds{180};
    /// Минимальный нотионал для определения пылевой позиции (USDT).
    double dust_threshold_usdt{0.50};
    /// Мин. профит в round-trip fees для quick profit (множитель). 5× = значимая прибыль.
    double quick_profit_fee_multiplier{5.0};
    /// Порог убытка для PnL gate (% от капитала). Ниже — не закрываем по сигналу.
    double pnl_gate_loss_pct{0.5};

    // ── Hedge Recovery ──
    /// Включить хеджирование убыточных позиций (locked position, затем закрытие проигрышной ноги)
    bool hedge_recovery_enabled{false};
    /// Порог убытка для активации хеджа (% от капитала). При 20x leverage 1.5% = серьёзный drawdown.
    double hedge_trigger_loss_pct{1.5};
    /// Мин. чистая прибыль (множитель от round-trip fees) для закрытия обеих ног.
    double hedge_profit_close_fee_mult{2.0};
};

/// Operational safety parameters — NOT alpha/trading logic.
/// These control system health gates, deadman watchdogs, and recovery behavior.
struct OperationalSafetyConfig {
    int feed_stale_ms{1200};                    ///< Market data считается stale после N мс
    int private_ws_gap_ms{2500};                ///< Private WS heartbeat gap threshold (мс)
    int order_state_desync_ms{5000};            ///< Порог рассинхронизации ордеров (мс)
    int orphan_leg_grace_ms{250};               ///< Grace window для unhedged leg (мс)
    bool block_trade_until_full_sync{true};     ///< Блокировать торговлю до полной синхронизации
    int venue_degraded_flatten_ms{8000};        ///< Flatten при деградации биржи (мс)
    int operational_deadman_minutes{45};         ///< Operational deadman: порог для проверки деградации (минуты)
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

    // ── Комиссии биржи (USDT-M futures) ──
    double taker_fee_bps{6.0};   ///< Taker комиссия [bps] (Bitget standard: 0.06%)
    double maker_fee_bps{2.0};   ///< Maker комиссия [bps] (Bitget standard: 0.02%)

    // ── EV-based style selection ──
    double opportunity_cost_bps{30.0};     ///< Edge lost (bps) when limit order doesn't fill
    double queue_depletion_penalty{0.08};  ///< Fill prob reduction when our-side queue depleting fast
    double churn_penalty{0.06};            ///< Fill prob reduction when top-of-book is unstable
    double feedback_weight{0.30};          ///< Weight for historical passive_fill_rate feedback
};

/// Настройки модуля opportunity cost
struct OpportunityCostConfig {
    // ── Пороги net edge (базисные пункты) ──
    double min_net_expected_bps{1.0};          ///< Мин чистый ожидаемый доход для входа
    double execute_min_net_bps{3.0};           ///< Мин чистый доход для немедленного исполнения

    // ── Пороги экспозиции ──
    double high_exposure_threshold{0.70};      ///< Порог высокой экспозиции [0,1]
    double high_exposure_min_conviction{0.60}; ///< Мин conviction при высокой экспозиции

    // ── Пороги концентрации ──
    double max_symbol_concentration{0.25};     ///< Макс доля капитала на один символ
    double max_strategy_concentration{0.35};   ///< Макс доля капитала на одну стратегию

    // ── Пороги капитала ──
    double capital_exhaustion_threshold{0.85}; ///< Порог исчерпания капитала [0,1]

    // ── Веса скоринга ──
    double weight_conviction{0.35};            ///< Вес conviction в composite score
    double weight_net_edge{0.35};              ///< Вес net edge
    double weight_capital_efficiency{0.15};    ///< Вес capital efficiency
    double weight_urgency{0.15};               ///< Вес urgency

    // ── Масштабирование expected return ──
    double conviction_to_bps_scale{100.0};     ///< Масштаб: conviction 1.0 → N bps

    // ── Upgrade ──
    double upgrade_min_edge_advantage_bps{5.0}; ///< Мин разница edge для Upgrade vs худшей позиции

    // ── Drawdown penalty ──
    double drawdown_penalty_scale{0.5};        ///< Множитель: +X к порогу за каждые 5% просадки

    // ── Consecutive loss penalty ──
    double consecutive_loss_penalty{0.02};     ///< +X к порогу за каждый убыточный трейд подряд
};

// ─── Фьючерсная торговля ──────────────────────────────────────────────────────

/// Конфигурация движка адаптивного плеча
///
/// Дефолты откалиброваны для USDT-M futures scalping на 1-минутных свечах.
/// Источники:
///   - Volatility: Parkinson (1980) high-low estimator; эмпирические данные BTC/ETH/SOL 1-min ATR
///   - Drawdown:   tanh-sigmoid (C∞-гладкость), Grossman & Zhou (1993) optimal leverage under drawdown
///   - Conviction: двухсегментная кривая с настраиваемым floor и ceiling
///   - EMA:        Brown (1956) exponential smoothing; α=0.3 ≈ 5-тиковый lookback
struct LeverageEngineConfig {
    // Volatility multiplier breakpoints (ATR/price ratio, 1-min candles)
    // Эмпирическая калибровка: BTC 1-min ATR/price ≈ 0.0005-0.003 (calm-elevated)
    double vol_low_atr{0.001};       ///< ≤ 0.1%: calm market, no penalty
    double vol_mid_atr{0.003};       ///< 0.3%: moderate vol → mult 0.7
    double vol_high_atr{0.008};      ///< 0.8%: significant vol → mult 0.4
    double vol_extreme_atr{0.02};    ///< 2.0%: flash-crash territory → mult 0.2
    double vol_floor{0.10};          ///< Минимальный множитель волатильности

    // Conviction multiplier
    double conviction_min_mult{0.40};    ///< Множитель при нулевом conviction
    double conviction_breakpoint{0.70};  ///< Порог перехода к бонусу (neutral point)
    double conviction_max_mult{1.30};    ///< Максимальный бонус за conviction

    // Drawdown curve (tanh sigmoid — Grossman & Zhou 1993)
    double drawdown_floor_mult{0.10};    ///< Минимум при max drawdown
    double drawdown_halfpoint_pct{10.0}; ///< DD% при 50% снижении

    // EMA smoothing (Brown 1956)
    double ema_alpha{0.3};               ///< Скорость реакции EMA (0.3 ≈ 5-tick lookback)

    // Fee rate for liquidation price calc (Bitget USDT-M taker fee)
    double taker_fee_rate{0.0006};       ///< 0.06% Bitget futures taker

    // Leverage change debounce
    int min_leverage_change_delta{2};    ///< Минимальное изменение для API call
};

/// Конфигурация фьючерсной торговли (USDT-M)
struct FuturesConfig {
    bool enabled{true};                        ///< Фьючерсный режим (бот ТОЛЬКО USDT-M фьючерсный)
    std::string product_type{"USDT-FUTURES"};  ///< Тип продукта Bitget (только USDT-M)
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

    // Защита от ликвидации
    double liquidation_buffer_pct{5.0};        ///< Мин. буфер до ликвидации (%)

    // Фандинг (Bitget 8-hour funding rate, decimal: 0.0001 = 0.01%)
    double funding_rate_threshold{0.0005};     ///< Порог: 0.05% per 8h (elevated funding)
    double funding_rate_penalty{0.5};          ///< Пенальти к плечу при высоком фандинге

    // Maintenance margin (Bitget default for small positions)
    double maintenance_margin_rate{0.004};     ///< Maintenance margin rate (0.4%)

    // Leverage engine config
    LeverageEngineConfig leverage_engine;     ///< Параметры адаптивного движка плеча
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
    WorldModelConfig     world_model;      ///< Настройки мировой модели
    FuturesConfig        futures;          ///< Настройки фьючерсной торговли
    UncertaintyConfig    uncertainty;      ///< Настройки модуля неопределённости
    OperationalSafetyConfig operational_safety; ///< Operational safety (deadman, sync gates)
    std::string          config_hash;      ///< SHA-256 хеш файла конфигурации (для аудита)
};

} // namespace tb::config
