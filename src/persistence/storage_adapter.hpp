#pragma once
/**
 * @file storage_adapter.hpp
 * @brief Абстракция хранилища — интерфейс адаптера
 */
#include "persistence/persistence_types.hpp"
#include "common/result.hpp"
#include <vector>
#include <optional>

namespace tb::persistence {

/// Абстрактный адаптер хранилища
class IStorageAdapter {
public:
    virtual ~IStorageAdapter() = default;

    /// Добавить запись в журнал событий
    virtual VoidResult append_journal(const JournalEntry& entry) = 0;

    /// Запросить записи из журнала по временному диапазону и опциональному фильтру типа
    virtual Result<std::vector<JournalEntry>> query_journal(
        Timestamp from, Timestamp to,
        std::optional<JournalEntryType> type_filter = std::nullopt) = 0;

    /// Сохранить снимок состояния
    virtual VoidResult store_snapshot(const SnapshotEntry& entry) = 0;

    /// Загрузить последний снимок указанного типа
    virtual Result<SnapshotEntry> load_latest_snapshot(SnapshotType type) = 0;

    /// Принудительный сброс буферов на диск
    virtual VoidResult flush() = 0;
};

} // namespace tb::persistence
