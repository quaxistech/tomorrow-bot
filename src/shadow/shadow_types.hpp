#pragma once
/**
 * @file shadow_types.hpp
 * @brief Типы shadow trading подсистемы — виртуальное исполнение, оценка
 *        и сравнение торговых решений без реальных ордеров (спот)
 */

#include "common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb::shadow {

// ============================================================
// Enums
// ============================================================

/// Режим работы shadow-подсистемы
enum class ShadowMode {
    Observation,  ///< Чистая запись — все сигналы записываются as-is
    Validation,   ///< Сравнение shadow vs live (дублирует live-пайплайн)
    Discovery     ///< Scenario exploration (альтернативные risk/execution)
};

/// Политика управления рисками в shadow-режиме
enum class ShadowRiskPolicy {
    MirrorLive,    ///< Shadow повторяет live risk limits
    Relaxed,       ///< Ослабленные лимиты (для исследования)
    Unconstrained  ///< Без risk limits (что-если анализ)
};

/// Явная спот-семантика торгового действия
enum class SignalIntent {
    LongEntry,       ///< Открытие длинной позиции (Buy)
    LongExit,        ///< Закрытие длинной позиции (Sell)
    ReducePosition,  ///< Частичное закрытие позиции
    Flatten          ///< Полное закрытие всех позиций
};

/// Lifecycle состояния shadow-ордера
enum class ShadowOrderState {
    Pending,
    Submitted,
    PartialFill,
    Filled,
    Cancelled,
    Rejected,
    Expired
};

/// Строковое представление ShadowOrderState
inline const char* to_string(ShadowOrderState s) noexcept {
    switch (s) {
        case ShadowOrderState::Pending:     return "Pending";
        case ShadowOrderState::Submitted:   return "Submitted";
        case ShadowOrderState::PartialFill: return "PartialFill";
        case ShadowOrderState::Filled:      return "Filled";
        case ShadowOrderState::Cancelled:   return "Cancelled";
        case ShadowOrderState::Rejected:    return "Rejected";
        case ShadowOrderState::Expired:     return "Expired";
    }
    return "Unknown";
}

// ============================================================
// Configuration
// ============================================================

/// Полная конфигурация shadow-подсистемы
struct ShadowConfig {
    bool enabled{false};
    ShadowMode mode{ShadowMode::Observation};
    ShadowRiskPolicy risk_policy{ShadowRiskPolicy::MirrorLive};

    int max_records_per_strategy{10000};

    // Окна оценки (наносекунды)
    int64_t eval_window_short_ns{1'000'000'000};        ///< 1 секунда
    int64_t eval_window_mid_ns{5'000'000'000};           ///< 5 секунд
    int64_t eval_window_long_ns{30'000'000'000};         ///< 30 секунд
    int64_t stale_record_timeout_ns{120'000'000'000};    ///< 120 секунд

    // Комиссии (Bitget spot)
    double taker_fee_pct{0.001};   ///< Taker fee 0.1%
    double maker_fee_pct{0.0008};  ///< Maker fee 0.08%

    bool persist_to_db{false};
    bool respect_kill_switch{true};

    // Пороги алертов
    double alert_pnl_divergence_bps{100.0};    ///< Порог дивергенции P&L (bps)
    double alert_hit_rate_divergence{0.10};     ///< Порог дивергенции hit rate
};

// ============================================================
// Core records
// ============================================================

/// Теневое решение с полным контекстом на момент принятия
struct ShadowDecision {
    CorrelationId correlation_id{""};
    StrategyId strategy_id{""};
    Symbol symbol{""};
    Side side{Side::Buy};
    SignalIntent signal_intent{SignalIntent::LongEntry};
    Quantity quantity{0.0};
    Price intended_price{0.0};
    double conviction{0.0};
    Timestamp decided_at{0};

    // Контекст на момент решения
    std::string world_state;
    std::string regime;
    std::string uncertainty_level;
    std::string risk_verdict;

    bool would_have_been_live{false};

    std::string feature_snapshot_json;   ///< Сериализованный контекст фич
    std::string risk_decision_json;      ///< Сериализованное решение risk-модуля
};

/// Снимок цен для окон оценки (заменяет жёстко зашитые price fields)
struct ShadowPriceSnapshot {
    std::optional<Price> price_at_short;   ///< Цена через ~1 с
    std::optional<Price> price_at_mid;     ///< Цена через ~5 с
    std::optional<Price> price_at_long;    ///< Цена через ~30 с
    bool is_complete{false};               ///< Все окна заполнены
    bool had_data_gap{false};              ///< Был пропуск рыночных данных
    Timestamp last_update{0};
};

/// Симуляция исполнения shadow-ордера
struct ShadowFillSimulation {
    Price simulated_fill_price{0.0};
    double estimated_slippage_bps{0.0};
    double entry_fee_bps{0.0};   ///< Комиссия входа (bps)
    double exit_fee_bps{0.0};    ///< Комиссия выхода (bps)
    ShadowOrderState order_state{ShadowOrderState::Pending};
};

/// Полная запись shadow-сделки
struct ShadowTradeRecord {
    ShadowDecision decision;
    ShadowPriceSnapshot price_tracking;
    ShadowFillSimulation fill_sim;

    Price market_price_at_decision{0.0};

    double gross_pnl_bps{0.0};  ///< P&L до комиссий
    double net_pnl_bps{0.0};    ///< P&L после комиссий

    bool tracking_complete{false};
    Timestamp completed_at{0};
};

// ============================================================
// Position tracking
// ============================================================

/// Одна нога (leg) shadow-позиции
struct ShadowPositionLeg {
    Symbol symbol{""};
    Side side{Side::Buy};
    Quantity quantity{0.0};
    Price fill_price{0.0};
    double fee_bps{0.0};
    Timestamp timestamp{0};
};

/// Агрегированная shadow-позиция по символу/стратегии
struct ShadowPosition {
    Symbol symbol{""};
    StrategyId strategy_id{""};

    std::vector<ShadowPositionLeg> entry_legs;
    std::optional<ShadowPositionLeg> exit_leg;

    double total_entry_notional{0.0};
    double weighted_entry_price{0.0};
    double unrealized_pnl_bps{0.0};
    double realized_pnl_bps{0.0};

    bool is_open{true};
    Timestamp opened_at{0};
    Timestamp closed_at{0};
};

// ============================================================
// Analytics & monitoring
// ============================================================

/// Расширенное сравнение shadow vs live
struct ShadowComparison {
    StrategyId strategy_id{""};

    int shadow_trades{0};
    int live_trades{0};

    double shadow_gross_pnl_bps{0.0};
    double shadow_net_pnl_bps{0.0};
    double live_pnl_bps{0.0};

    double shadow_hit_rate{0.0};   ///< [0, 1]
    double live_hit_rate{0.0};     ///< [0, 1]

    double pnl_correlation{0.0};
    double max_drawdown_bps{0.0};

    Timestamp period_start{0};
    Timestamp period_end{0};

    std::vector<std::string> divergence_reasons;
};

/// Алерт, генерируемый shadow-подсистемой
struct ShadowAlert {
    StrategyId strategy_id{""};
    std::string alert_type;
    std::string severity;      ///< "info" | "warn" | "critical"
    std::string message;
    Timestamp detected_at{0};
    double shadow_value{0.0};
    double live_value{0.0};
};

/// Агрегированные метрики shadow-подсистемы
struct ShadowMetricsSummary {
    int total_decisions{0};
    int completed_decisions{0};
    int incomplete_decisions{0};

    double gross_pnl_bps{0.0};
    double net_pnl_bps{0.0};
    double hit_rate{0.0};
    double avg_trade_pnl_bps{0.0};

    double max_drawdown_bps{0.0};
    double sharpe_estimate{0.0};

    int decisions_blocked_by_risk{0};
    int data_gap_count{0};
};

} // namespace tb::shadow
