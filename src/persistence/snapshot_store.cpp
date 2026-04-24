/**
 * @file snapshot_store.cpp
 * @brief Реализация хранилища снимков состояний
 */
#include "persistence/snapshot_store.hpp"

namespace tb::persistence {

SnapshotStore::SnapshotStore(std::shared_ptr<IStorageAdapter> adapter)
    : adapter_(std::move(adapter)) {}

VoidResult SnapshotStore::save(
    SnapshotType type,
    const std::string& payload_json,
    const ConfigHash& config_hash) {

    // Текущее время в наносекундах (system_clock for cross-restart portability)
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    std::lock_guard lock(mutex_);
    // Генерация монотонного snapshot_id под мьютексом, чтобы сохранялся
    // тот же порядок, в котором записи попадают в adapter.
    uint64_t id = snapshot_counter_.fetch_add(1, std::memory_order_relaxed) + 1;

    SnapshotEntry entry;
    entry.snapshot_id = id;
    entry.type = type;
    entry.created_at = Timestamp{ns};
    entry.config_hash = config_hash;
    entry.payload_json = payload_json;

    return adapter_->store_snapshot(entry);
}

Result<SnapshotEntry> SnapshotStore::load_latest(SnapshotType type) {
    std::lock_guard lock(mutex_);
    return adapter_->load_latest_snapshot(type);
}

VoidResult SnapshotStore::flush() {
    std::lock_guard lock(mutex_);
    return adapter_->flush();
}

} // namespace tb::persistence
