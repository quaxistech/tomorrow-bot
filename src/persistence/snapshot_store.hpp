#pragma once
/**
 * @file snapshot_store.hpp
 * @brief Хранилище снимков состояний — обёртка над адаптером
 */
#include "persistence/storage_adapter.hpp"
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>

namespace tb::persistence {

/// Хранилище снимков с автоматической генерацией snapshot_id
class SnapshotStore {
public:
    /// Конструктор принимает адаптер хранилища
    explicit SnapshotStore(std::shared_ptr<IStorageAdapter> adapter);

    /// Сохранить снимок состояния
    VoidResult save(
        SnapshotType type,
        const std::string& payload_json,
        const ConfigHash& config_hash = ConfigHash{""});

    /// Загрузить последний снимок указанного типа
    Result<SnapshotEntry> load_latest(SnapshotType type);

    /// Принудительный сброс буферов
    VoidResult flush();

private:
    std::shared_ptr<IStorageAdapter> adapter_;
    std::atomic<uint64_t> snapshot_counter_{0};
    std::mutex mutex_;
};

} // namespace tb::persistence
