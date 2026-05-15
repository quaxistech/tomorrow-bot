/**
 * @file memory_storage_adapter.cpp
 * @brief Реализация in-memory адаптера хранилища
 */
#include "persistence/memory_storage_adapter.hpp"
#include <algorithm>

namespace tb::persistence {

VoidResult MemoryStorageAdapter::append_journal(const JournalEntry& entry) {
    std::lock_guard lock(mutex_);
    journal_entries_.push_back(entry);
    return OkVoid();
}

Result<std::vector<JournalEntry>> MemoryStorageAdapter::query_journal(
    Timestamp from, Timestamp to,
    std::optional<JournalEntryType> type_filter) {

    std::lock_guard lock(mutex_);
    std::vector<JournalEntry> result;

    for (const auto& entry : journal_entries_) {
        // Фильтрация по временному диапазону
        if (entry.timestamp.get() < from.get() || entry.timestamp.get() > to.get()) {
            continue;
        }
        // Фильтрация по типу (если задан)
        if (type_filter.has_value() && entry.type != *type_filter) {
            continue;
        }
        result.push_back(entry);
    }

    return Ok(std::move(result));
}

VoidResult MemoryStorageAdapter::store_snapshot(const SnapshotEntry& entry) {
    std::lock_guard lock(mutex_);
    int key = static_cast<int>(entry.type);
    snapshots_[key] = entry;
    return OkVoid();
}

Result<SnapshotEntry> MemoryStorageAdapter::load_latest_snapshot(SnapshotType type) {
    std::lock_guard lock(mutex_);
    int key = static_cast<int>(type);
    auto it = snapshots_.find(key);
    if (it == snapshots_.end()) {
        return Err<SnapshotEntry>(TbError::PersistenceError);
    }
    return Ok(SnapshotEntry{it->second});
}

VoidResult MemoryStorageAdapter::flush() {
    // In-memory — сброс не требуется
    return OkVoid();
}

size_t MemoryStorageAdapter::journal_size() const {
    std::lock_guard lock(mutex_);
    return journal_entries_.size();
}

size_t MemoryStorageAdapter::snapshot_count() const {
    std::lock_guard lock(mutex_);
    return snapshots_.size();
}

} // namespace tb::persistence
