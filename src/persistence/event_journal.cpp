/**
 * @file event_journal.cpp
 * @brief Реализация журнала событий
 */
#include "persistence/event_journal.hpp"

namespace tb::persistence {

EventJournal::EventJournal(std::shared_ptr<IStorageAdapter> adapter)
    : adapter_(std::move(adapter)) {}

VoidResult EventJournal::append(
    JournalEntryType type,
    const std::string& payload_json,
    const CorrelationId& correlation_id,
    const StrategyId& strategy_id,
    const ConfigHash& config_hash) {

    // Генерация монотонного sequence_id (атомарно)
    uint64_t seq = sequence_counter_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Текущее время в наносекундах
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    JournalEntry entry;
    entry.sequence_id = seq;
    entry.type = type;
    entry.timestamp = Timestamp{ns};
    entry.correlation_id = correlation_id;
    entry.strategy_id = strategy_id;
    entry.config_hash = config_hash;
    entry.payload_json = payload_json;

    std::lock_guard lock(mutex_);
    return adapter_->append_journal(entry);
}

Result<std::vector<JournalEntry>> EventJournal::query(
    Timestamp from, Timestamp to,
    std::optional<JournalEntryType> type_filter) {

    std::lock_guard lock(mutex_);
    return adapter_->query_journal(from, to, type_filter);
}

VoidResult EventJournal::flush() {
    std::lock_guard lock(mutex_);
    return adapter_->flush();
}

} // namespace tb::persistence
