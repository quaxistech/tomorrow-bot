#pragma once
/**
 * @file recovery_types.hpp
 * @brief Типы и конфигурация модуля восстановления состояния (USDT-M Futures)
 *
 * Описывает статусы recovery, записи о восстановленных
 * фьючерсных позициях и итоговый результат.
 */

#include "common/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tb::recovery {

// ============================================================
// Перечисления
// ============================================================

/// Статус восстановления
enum class RecoveryStatus {
    NotStarted,
    InProgress,
    Completed,
    CompletedWithWarnings,
    Failed
};

// ============================================================
// to_string
// ============================================================

[[nodiscard]] inline constexpr std::string_view to_string(RecoveryStatus s) noexcept {
    switch (s) {
        case RecoveryStatus::NotStarted:            return "NotStarted";
        case RecoveryStatus::InProgress:            return "InProgress";
        case RecoveryStatus::Completed:             return "Completed";
        case RecoveryStatus::CompletedWithWarnings: return "CompletedWithWarnings";
        case RecoveryStatus::Failed:                return "Failed";
    }
    return "Unknown";
}

// ============================================================
// Записи о восстановленных объектах
// ============================================================

/// Запись о восстановленной фьючерсной позиции
struct RecoveredPosition {
    Symbol symbol{""};
    Side side{Side::Buy};                        ///< Buy = Long, Sell = Short
    Quantity size{0.0};
    Price avg_entry_price{0.0};
    double estimated_pnl{0.0};
    bool had_matching_strategy{false};           ///< Была ли стратегия для этой позиции
    std::string resolution;                      ///< Что было сделано
};

// ============================================================
// Результат и конфигурация
// ============================================================

/// Результат восстановления
struct RecoveryResult {
    RecoveryStatus status{RecoveryStatus::NotStarted};
    std::vector<RecoveredPosition> recovered_positions;
    double recovered_cash_balance{0.0};
    double balance_adjustment{0.0};
    int warnings{0};
    int errors{0};
    int64_t duration_ms{0};
    Timestamp completed_at{Timestamp{0}};
    std::vector<std::string> messages;
};

/// Конфигурация recovery (USDT-M Futures)
///
/// Значения по умолчанию основаны на:
/// - Bitget USDT-M Futures API v2 contract specifications
/// - Almgren & Chriss (2000) "Optimal Execution of Portfolio Transactions"
///   для рекомендаций по threshold-based reconciliation
struct RecoveryConfig {
    bool enabled{true};

    /// Не синхронизировать orphan-позиции в портфель (только логировать).
    /// false (default) = синхронизировать в портфель для отслеживания.
    /// true = пропустить, только предупреждение в лог.
    bool close_orphan_positions{false};

    /// Минимальная стоимость позиции (USD) для восстановления.
    /// Позиции ниже этого порога считаются пылевыми и пропускаются.
    /// Bitget USDT-M minimum notional: $5 (per contract specifications).
    double min_position_value_usd{5.0};

    /// Ограничить recovery конкретным символом (пустой = все).
    Symbol symbol_filter{Symbol("")};
};

// ============================================================
// Расширенные типы для deterministic recovery (Phase 6)
// — обеспечивают полное восстановление pair-state, pending orders,
//   protective TP/SL после рестарта / reconnect
// ============================================================

/// Восстановленный pending (working) ордер на бирже
struct RecoveredPendingOrder {
    OrderId order_id{""};
    Symbol symbol{""};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};
    double price{0.0};
    double remaining_qty{0.0};
    std::string order_type;          ///< "limit", "market", "trigger"
    bool is_reduce_only{false};
    std::string exchange_order_id;
    std::string resolution;          ///< "adopted" | "cancelled" | "orphan"
};

/// Восстановленный protective ордер (TP/SL)
struct RecoveredProtectiveOrder {
    OrderId order_id{""};
    Symbol symbol{""};
    PositionSide position_side{PositionSide::Long};
    double trigger_price{0.0};
    bool is_tp{false};               ///< true = take-profit, false = stop-loss
    bool still_active{false};        ///< Жив ли на бирже
    std::string resolution;
};

/// Восстановленное состояние пары primary + hedge
struct RecoveredPairState {
    Symbol symbol{""};
    bool has_primary{false};
    bool has_hedge{false};
    Side primary_side{Side::Buy};
    double primary_size{0.0};
    double hedge_size{0.0};
    double primary_entry_price{0.0};
    double hedge_entry_price{0.0};
    std::string inferred_state;      ///< "PrimaryOnly" | "PrimaryPlusHedge" | etc.
    std::string resolution;
};

/// Расширенный результат deterministic recovery
struct ExtendedRecoveryResult {
    RecoveryResult base;

    std::vector<RecoveredPendingOrder> pending_orders;
    std::vector<RecoveredProtectiveOrder> protective_orders;
    std::vector<RecoveredPairState> pair_states;

    int pending_orders_adopted{0};
    int pending_orders_cancelled{0};
    int protective_orders_verified{0};
    int protective_orders_missing{0};
    int pair_states_restored{0};
};

} // namespace tb::recovery
