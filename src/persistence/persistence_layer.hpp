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
#include <mutex>

namespace tb::persistence {

/// Фасад слоя персистентности
class PersistenceLayer {
public:
    /// Конструктор принимает адаптер хранилища и опциональную конфигурацию
    explicit PersistenceLayer(
        std::shared_ptr<IStorageAdapter> adapter,
        PersistenceConfig config = {});

    /// Получить ссылку на журнал событий
    [[nodiscard]] EventJournal& journal();

    /// Получить ссылку на хранилище снимков
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
    mutable std::mutex mutex_;
};

} // namespace tb::persistence
