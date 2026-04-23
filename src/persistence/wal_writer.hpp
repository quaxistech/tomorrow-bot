#pragma once
/**
 * @file wal_writer.hpp
 * @brief Write-Ahead Logger — синхронная запись критических событий ДО изменения in-memory state
 *
 * WAL обеспечивает, что каждое критическое действие (открытие/закрытие позиции,
 * отправка ордера) сначала записывается в журнал, и только потом выполняется.
 * При crash-recovery WAL проигрывается и незавершённые действия обнаруживаются.
 */
#include "persistence/event_journal.hpp"
#include "persistence/persistence_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "common/types.hpp"
#include "common/result.hpp"
#include <memory>
#include <mutex>
#include <atomic>

namespace tb::persistence {

/// Тип WAL записи
enum class WalEntryType {
    OrderIntent,         ///< Намерение отправить ордер (ДО отправки)
    OrderSent,           ///< Ордер отправлен (ПОСЛЕ подтверждения)
    OrderCancelled,      ///< Ордер отменён
    PositionOpened,      ///< Позиция открыта
    PositionClosed,      ///< Позиция закрыта
    BalanceSync,         ///< Синхронизация баланса
    RecoveryCheckpoint   ///< Контрольная точка для recovery
};

/// to_string for WalEntryType
[[nodiscard]] std::string to_string(WalEntryType type);

/// WAL запись
struct WalEntry {
    uint64_t wal_sequence{0};
    WalEntryType type;
    std::string payload_json;
    CorrelationId correlation_id{""};
    OrderId order_id{""};
    Symbol symbol{""};
    StrategyId strategy_id{""};
    Timestamp written_at{0};
    bool committed{false};          ///< true = действие завершено успешно
};

/// Write-Ahead Logger
class WalWriter {
public:
    WalWriter(
        std::shared_ptr<EventJournal> journal,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /// Записать намерение (intent) ПЕРЕД выполнением действия.
    /// Возвращает wal_sequence для последующего commit.
    Result<uint64_t> write_intent(
        WalEntryType type,
        const std::string& payload_json,
        const OrderId& order_id = OrderId(""),
        const Symbol& symbol = Symbol(""),
        const StrategyId& strategy_id = StrategyId(""),
        const CorrelationId& correlation_id = CorrelationId(""));

    /// Подтвердить выполнение действия (commit)
    VoidResult commit(uint64_t wal_sequence);

    /// Откатить (rollback) — действие не выполнено
    VoidResult rollback(uint64_t wal_sequence, const std::string& reason);

    /// Найти незавершённые (uncommitted) записи — для recovery
    Result<std::vector<WalEntry>> find_uncommitted();

    /// Записать контрольную точку
    VoidResult write_checkpoint(const std::string& snapshot_json);

    /// Принудительный flush на диск
    VoidResult flush();

private:
    std::shared_ptr<EventJournal> journal_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    std::atomic<uint64_t> wal_sequence_{0};
    std::atomic<uint64_t> wal_corruption_count_{0};
    std::unordered_map<uint64_t, WalEntry> pending_entries_;
    mutable std::mutex mutex_;
};

} // namespace tb::persistence
