#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::risk {

// ═══════════════════════════════════════════════════════════════
// Enums
// ═══════════════════════════════════════════════════════════════

/// Решение риск-движка (legacy-совместимый вердикт)
enum class RiskVerdict {
    Approved,
    Denied,
    ReduceSize,
    Throttled
};

/// Расширенное действие риск-движка (policy-based)
enum class RiskAction {
    Allow,
    AllowWithReducedSize,
    Deny,
    DenySymbolLock,
    DenyStrategyLock,
    DenyDayLock,
    DenyAccountLock,
    EmergencyHalt
};

/// Фаза оценки риска
enum class RiskPhase {
    PreTrade,
    IntraTrade,
    PostTrade
};

/// Глобальный уровень состояния риска
enum class RiskStateLevel {
    Normal,
    Degraded,
    DayLock,
    SymbolLock,
    StrategyLock,
    AccountLock,
    EmergencyHalt
};

/// Тип блокировки
enum class LockType {
    SymbolLock,
    StrategyLock,
    DayLock,
    AccountLock,
    EmergencyHalt,
    Cooldown
};

// ═══════════════════════════════════════════════════════════════
// DTOs
// ═══════════════════════════════════════════════════════════════

/// Причина решения риск-движка
struct RiskReasonCode {
    std::string code;
    std::string message;
    double severity{0.0};    ///< [0=предупреждение, 1=абсолютный отказ]
};

/// Запись о блокировке
struct LockRecord {
    LockType type{LockType::Cooldown};
    std::string target;      ///< Символ / стратегия / "" для аккаунта
    std::string reason;
    Timestamp activated_at{Timestamp(0)};
    int64_t duration_ns{0};  ///< 0 = бессрочная
};

/// Полное решение риск-движка (расширенное policy-based)
struct RiskDecision {
    // === Legacy-совместимые поля ===
    RiskVerdict verdict{RiskVerdict::Denied};
    Quantity approved_quantity{Quantity(0.0)};
    double risk_utilization_pct{0.0};
    bool kill_switch_active{false};
    Timestamp decided_at{Timestamp(0)};
    std::string summary;
    RiskPhase phase{RiskPhase::PreTrade};
    double regime_scaling_factor{1.0};
    double strategy_budget_utilization_pct{0.0};
    double symbol_concentration_pct{0.0};

    // === Расширенные поля (policy-based) ===
    RiskAction action{RiskAction::Deny};
    bool allowed{false};
    Quantity original_size{Quantity(0.0)};
    RiskStateLevel risk_state{RiskStateLevel::Normal};

    std::vector<std::string> triggered_checks;
    std::vector<std::string> warnings;
    std::vector<std::string> hard_blocks;

    // Структурированные причины
    std::vector<RiskReasonCode> reasons;

    // Аудит
    double current_daily_pnl{0.0};
    double current_drawdown_pct{0.0};
    double current_gross_exposure{0.0};
    int active_locks_count{0};
};

/// Бюджет риска одной стратегии
struct StrategyRiskBudget {
    StrategyId strategy_id{StrategyId("")};
    double daily_loss{0.0};
    double daily_loss_pct{0.0};
    double current_exposure{0.0};
    int trades_today{0};
    int consecutive_losses{0};
    Timestamp last_trade_at{Timestamp(0)};
};

/// Снимок состояния риск-движка
struct RiskSnapshot {
    double total_risk_utilization{0.0};
    double daily_loss_pct{0.0};
    double current_drawdown_pct{0.0};
    int open_positions{0};
    double gross_exposure_pct{0.0};
    bool kill_switch_active{false};
    double regime_scaling_factor{1.0};
    int rules_triggered{0};
    RiskStateLevel global_state{RiskStateLevel::Normal};
    std::vector<StrategyRiskBudget> strategy_budgets;
    std::vector<LockRecord> active_locks;
    Timestamp computed_at{Timestamp(0)};
};

/// Результат intra-trade мониторинга позиции
struct IntraTradeAssessment {
    Symbol symbol{Symbol("")};
    bool should_close{false};
    bool should_reduce{false};
    double reduce_fraction{0.0};
    std::vector<RiskReasonCode> reasons;
    Timestamp assessed_at{Timestamp(0)};
};

/// Конфигурация лимитов риска (USDT-M futures scalping)
///
/// Значения по умолчанию основаны на:
/// - Стандарты управления рисками для алгоритмической торговли (2% правило Kelly-fraction)
/// - Эмпирические параметры ликвидности USDT-M perpetual futures на Bitget
/// - Регуляторные рекомендации по макс. плечу для ритейл-фьючерсов
struct ExtendedRiskConfig {
    // === Per-trade ===
    double max_position_notional{10000.0};      ///< Макс. номинал одной позиции (USDT)
    double max_loss_per_trade_pct{1.0};         ///< Макс. убыток по 1 сделке как % капитала (Kelly ½f*)
    double max_leverage{20.0};                  ///< Макс. кредитное плечо (Bitget limit: 125x, conservative: 20x)
    double max_slippage_bps{30.0};              ///< Макс. проскальзывание (30 bps = 0.3%, empirical top-20 pairs)

    // === Per-symbol ===
    double max_daily_loss_per_symbol_pct{3.0};  ///< Макс. дневной убыток по 1 символу (% капитала)
    int max_consecutive_losses_per_symbol{3};   ///< Серия стопаутов по символу до cooldown
    int64_t symbol_cooldown_after_stopouts_ns{120'000'000'000LL}; ///< Cooldown после серии (2 мин)

    // === Per-strategy ===
    double max_strategy_daily_loss_pct{1.5};    ///< Макс. gross-loss бюджет стратегии за день (% капитала)

    // === Daily ===
    double max_daily_loss_pct{2.0};             ///< Макс. дневной убыток total PnL (% капитала, Kelly: 2%)
    double max_realized_daily_loss_pct{1.5};    ///< Макс. реализованный дневной убыток (% капитала)

    // === Drawdown ===
    double max_drawdown_pct{5.0};               ///< Макс. drawdown от peak equity (% — стандарт: 5-10%)
    double max_intraday_drawdown_pct{3.0};      ///< Макс. внутридневная просадка (%)
    double drawdown_hard_stop_pct{10.0};        ///< Emergency halt при критической просадке (%)

    // === Positions ===
    int max_concurrent_positions{5};            ///< Макс. одновременных позиций
    int max_simultaneous_long_positions{3};     ///< Макс. одновременных long
    int max_simultaneous_short_positions{3};    ///< Макс. одновременных short

    // === Exposure ===
    double max_gross_exposure_pct{50.0};        ///< Макс. gross exposure как % капитала (post-trade projected)
    double max_symbol_concentration_pct{35.0};  ///< Макс. концентрация 1 символа (post-trade projected, %)

    // === Consecutive losses ===
    int max_consecutive_losses{5};              ///< Порог серии убытков → deny
    int cooldown_after_n_losses{3};             ///< Глобальный cooldown после N подряд убытков
    int64_t loss_cooldown_ns{60'000'000'000LL}; ///< Длительность глобального cooldown (60с)
    int halt_after_n_losses{8};                 ///< DayLock после N подряд убытков

    // === Market conditions ===
    double max_spread_bps{50.0};                ///< Макс. спред (50 bps, P95 для top-20 USDT-M pairs)
    double min_liquidity_depth{100.0};          ///< Мин. суммарная L5 depth (USDT)
    int64_t max_feed_age_ns{5'000'000'000LL};   ///< Макс. возраст market data (5с)

    // === Rate limiting ===
    int max_orders_per_minute{10};              ///< Макс. ордеров/мин (Bitget rate limit: 20/s)
    int max_trades_per_hour{8};                 ///< Макс. сделок/час (overtrading protection)
    int64_t min_trade_interval_ns{30'000'000'000LL}; ///< Мин. интервал между сделками по символу (30с)

    // === Regime-aware ===
    bool regime_aware_limits_enabled{true};
    double stress_regime_scale{0.5};            ///< Scaling factor при stress-режиме (0.5 = -50% size)
    double trending_regime_scale{1.2};          ///< Scaling при trending (1.2 = +20% size)
    double chop_regime_scale{0.7};              ///< Scaling при чоппи-рынке (0.7 = -30% size)

    // === Intra-trade ===
    double max_adverse_excursion_pct{3.0};      ///< MAE: макс. неблагоприятное отклонение (% капитала)
    int64_t max_position_hold_ns{3'600'000'000'000LL}; ///< Макс. время удержания (1 час, scalping)
    int64_t post_loss_cooldown_ns{60'000'000'000LL};   ///< Cooldown после убытка (60с)

    // === Kill switch ===
    bool kill_switch_enabled{true};

    // === Directional / same-direction ===
    int max_same_direction_positions{3};        ///< Макс. позиций в одном направлении

    // === Liquidation safety ===
    double liquidation_buffer_pct{5.0};         ///< Буфер до ликвидации для MaxLeverageCheck (%)

    // === Funding rate cost (USDT-M perpetual futures) ===
    double max_annual_funding_cost_pct{30.0};   ///< Макс. годовая стоимость фандинга (%)

    // === UTC cutoff ===
    int utc_cutoff_hour{-1};                    ///< Час UTC для прекращения торговли (-1 = отключено)
};

// ═══════════════════════════════════════════════════════════════
// to_string
// ═══════════════════════════════════════════════════════════════

std::string to_string(RiskVerdict verdict);
std::string to_string(RiskPhase phase);
std::string to_string(RiskAction action);
std::string to_string(RiskStateLevel level);
std::string to_string(LockType type);

} // namespace tb::risk
