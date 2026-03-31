/**
 * @file persistence_layer.cpp
 * @brief Реализация фасада слоя персистентности
 */
#include "persistence/persistence_layer.hpp"

namespace tb::persistence {

PersistenceLayer::PersistenceLayer(
    std::shared_ptr<IStorageAdapter> adapter,
    PersistenceConfig config)
    : adapter_(adapter)
    , config_(std::move(config))
    , journal_(adapter)
    , snapshots_(adapter) {}

EventJournal& PersistenceLayer::journal() {
    return journal_;
}

SnapshotStore& PersistenceLayer::snapshots() {
    return snapshots_;
}

VoidResult PersistenceLayer::flush() {
    // No facade-level lock: EventJournal and SnapshotStore each have their own
    // internal mutex, so concurrent flush() / append() / save() calls are safe.
    auto r1 = journal_.flush();
    if (!r1) return r1;
    return snapshots_.flush();
}

bool PersistenceLayer::is_enabled() const {
    return config_.enabled;
}

} // namespace tb::persistence
