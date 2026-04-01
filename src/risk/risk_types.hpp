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
    EmergencyHalt,
    ForceReduce,
    ForceLiquidate
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

/// Конфигурация лимитов риска (полная)
struct ExtendedRiskConfig {
    // === Per-trade ===
    double max_position_notional{10000.0};
    double max_risk_per_trade_pct{2.0};
    double max_risk_per_trade_abs{200.0};
    double max_loss_per_trade_pct{1.0};
    double max_leverage{3.0};
    double max_slippage_bps{30.0};

    // === Per-symbol ===
    double max_position_notional_per_symbol{10000.0};
    double max_daily_loss_per_symbol_abs{500.0};
    double max_daily_loss_per_symbol_pct{3.0};
    int max_consecutive_losses_per_symbol{3};
    int64_t symbol_cooldown_after_stopouts_ns{120'000'000'000LL}; // 2 мин

    // === Per-strategy ===
    double max_strategy_daily_loss_pct{1.5};
    double max_strategy_drawdown_pct{5.0};
    double max_strategy_exposure_pct{30.0};
    int max_positions_per_strategy{3};

    // === Daily ===
    double max_daily_loss_pct{2.0};
    double max_daily_loss_abs{500.0};
    double max_realized_daily_loss_pct{1.5};
    int max_daily_stopouts{5};

    // === Drawdown ===
    double max_drawdown_pct{5.0};
    double max_intraday_drawdown_pct{3.0};
    double drawdown_hard_stop_pct{10.0};

    // === Positions ===
    int max_concurrent_positions{5};
    int max_simultaneous_long_positions{3};
    int max_simultaneous_short_positions{3};

    // === Exposure ===
    double max_gross_exposure_pct{50.0};
    double max_net_directional_exposure_pct{30.0};
    double max_symbol_concentration_pct{35.0};

    // === Consecutive losses ===
    int max_consecutive_losses{5};
    int cooldown_after_n_losses{3};
    int64_t loss_cooldown_ns{60'000'000'000LL};  // 60с
    int halt_after_n_losses{8};

    // === Market conditions ===
    double max_spread_bps{50.0};
    double min_liquidity_depth{100.0};
    int64_t max_feed_age_ns{5'000'000'000LL};  // 5с

    // === Rate limiting ===
    int max_orders_per_minute{10};
    int max_trades_per_hour{8};
    int64_t min_trade_interval_ns{30'000'000'000LL}; // 30с

    // === Regime-aware ===
    bool regime_aware_limits_enabled{true};
    double stress_regime_scale{0.5};
    double trending_regime_scale{1.2};
    double chop_regime_scale{0.7};

    // === Intra-trade ===
    double max_adverse_excursion_pct{3.0};
    int64_t max_position_hold_ns{3'600'000'000'000LL}; // 1 час
    int64_t post_loss_cooldown_ns{60'000'000'000LL};   // 60с

    // === Kill switch ===
    bool kill_switch_enabled{true};
    bool kill_switch_on_data_stale{true};
    bool kill_switch_on_position_mismatch{true};

    // === Directional / same-direction ===
    int max_same_direction_positions{3};

    // === UTC cutoff ===
    int utc_cutoff_hour{-1};

    // === Fail-safe ===
    bool allow_reduce_only_in_halt{true};
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
