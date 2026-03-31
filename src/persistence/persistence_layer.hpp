#pragma once
/**
 * @file persistence_layer.hpp
 * @brief Фасад слоя персистентности — журнал событий и снимки состояний
 *
 * Объединяет EventJournal и SnapshotStore за единым интерфейсом.
 */
#include "persistence/event_journal.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/storage_adapter.hpp"
#include "persistence/persistence_types.hpp"
#include "common/result.hpp"
#include <memory>

namespace tb::persistence {

/// Фасад слоя персистентности
///
/// Thread safety: EventJournal and SnapshotStore are individually thread-safe
/// (each has its own internal mutex).  Callers may use journal() and snapshots()
/// concurrently without additional synchronisation at the facade level.
class PersistenceLayer {
public:
    /// Конструктор принимает адаптер хранилища и опциональную конфигурацию
    explicit PersistenceLayer(
        std::shared_ptr<IStorageAdapter> adapter,
        PersistenceConfig config = {});

    /// Получить ссылку на журнал событий
    /// @note EventJournal is internally thread-safe; no external locking needed
    [[nodiscard]] EventJournal& journal();

    /// Получить ссылку на хранилище снимков
    /// @note SnapshotStore is internally thread-safe; no external locking needed
    [[nodiscard]] SnapshotStore& snapshots();

    /// Сбросить буферы журнала и снимков на диск
    VoidResult flush();

    /// Проверка — включена ли персистентность
    [[nodiscard]] bool is_enabled() const;

private:
    std::shared_ptr<IStorageAdapter> adapter_;
    PersistenceConfig config_;
    EventJournal journal_;
    SnapshotStore snapshots_;
};

} // namespace tb::persistence
