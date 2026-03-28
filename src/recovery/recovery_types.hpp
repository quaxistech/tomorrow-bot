#pragma once
/**
 * @file recovery_types.hpp
 * @brief Типы и конфигурация модуля восстановления состояния
 *
 * Описывает режимы восстановления, статусы, записи о
 * восстановленных позициях/ордерах и итоговый результат.
 */

#include "common/types.hpp"
#include "execution/order_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tb::recovery {

// ============================================================
// Перечисления
// ============================================================

/// Режим восстановления
enum class RecoveryMode {
    Full,           ///< Полное восстановление (ордера + позиции + баланс)
    OrdersOnly,     ///< Только ордера
    PositionsOnly,  ///< Только позиции
    BalanceOnly     ///< Только баланс
};

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

[[nodiscard]] inline constexpr std::string_view to_string(RecoveryMode m) noexcept {
    switch (m) {
        case RecoveryMode::Full:          return "Full";
        case RecoveryMode::OrdersOnly:    return "OrdersOnly";
        case RecoveryMode::PositionsOnly: return "PositionsOnly";
        case RecoveryMode::BalanceOnly:   return "BalanceOnly";
    }
    return "Unknown";
}

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

/// Запись о восстановленной позиции
struct RecoveredPosition {
    Symbol symbol{""};
    Side side{Side::Buy};
    Quantity size{0.0};
    Price avg_entry_price{0.0};
    double estimated_pnl{0.0};
    bool had_matching_strategy{false};   ///< Была ли стратегия для этой позиции
    std::string resolution;              ///< Что было сделано
};

/// Запись о восстановленном ордере
struct RecoveredOrder {
    OrderId order_id{""};
    OrderId exchange_order_id{""};
    Symbol symbol{""};
    execution::OrderState exchange_state{execution::OrderState::New};
    execution::OrderState local_state_before{execution::OrderState::New};
    std::string resolution;
};

// ============================================================
// Результат и конфигурация
// ============================================================

/// Результат восстановления
struct RecoveryResult {
    RecoveryStatus status{RecoveryStatus::NotStarted};
    std::vector<RecoveredPosition> recovered_positions;
    std::vector<RecoveredOrder> recovered_orders;
    double recovered_cash_balance{0.0};
    double balance_adjustment{0.0};
    int warnings{0};
    int errors{0};
    int64_t duration_ms{0};
    Timestamp completed_at{Timestamp{0}};
    std::vector<std::string> messages;
};

/// Конфигурация recovery
struct RecoveryConfig {
    bool enabled{true};
    bool close_orphan_positions{false};        ///< Закрыть позиции без стратегий (осторожно!)
    bool cancel_stale_orders{true};            ///< Отменить старые ордера при recovery
    int64_t stale_order_age_ms{7200000};       ///< 2 часа — порог «старости» ордера
    double min_position_value_usd{1.0};        ///< Игнорировать пылевые позиции
    int max_recovery_attempts{3};
};

} // namespace tb::recovery
