#pragma once
/**
 * @file memory_storage_adapter.hpp
 * @brief In-memory адаптер хранилища — для тестов и начальной реализации
 */
#include "persistence/storage_adapter.hpp"
#include <vector>
#include <unordered_map>
#include <mutex>

namespace tb::persistence {

/// Адаптер хранилища в оперативной памяти (без файлового ввода-вывода)
class MemoryStorageAdapter final : public IStorageAdapter {
public:
    MemoryStorageAdapter() = default;
    ~MemoryStorageAdapter() override = default;

    /// Добавить запись в журнал
    VoidResult append_journal(const JournalEntry& entry) override;

    /// Запросить записи по временному диапазону с опциональным фильтром
    Result<std::vector<JournalEntry>> query_journal(
        Timestamp from, Timestamp to,
        std::optional<JournalEntryType> type_filter = std::nullopt) override;

    /// Сохранить снимок состояния
    VoidResult store_snapshot(const SnapshotEntry& entry) override;

    /// Загрузить последний снимок указанного типа
    Result<SnapshotEntry> load_latest_snapshot(SnapshotType type) override;

    /// Сброс буферов (noop для in-memory)
    VoidResult flush() override;

    /// Получить количество записей в журнале (вспомогательный метод для тестов)
    [[nodiscard]] size_t journal_size() const;

    /// Получить количество снимков (вспомогательный метод для тестов)
    [[nodiscard]] size_t snapshot_count() const;

private:
    mutable std::mutex mutex_;
    std::vector<JournalEntry> journal_entries_;
    /// Ключ — целочисленное значение SnapshotType
    std::unordered_map<int, SnapshotEntry> snapshots_;
};

} // namespace tb::persistence
