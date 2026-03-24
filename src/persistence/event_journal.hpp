#pragma once
/**
 * @file event_journal.hpp
 * @brief Журнал событий — обёртка над адаптером хранилища
 */
#include "persistence/storage_adapter.hpp"
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>

namespace tb::persistence {

/// Журнал событий с автоматической генерацией sequence_id
class EventJournal {
public:
    /// Конструктор принимает адаптер хранилища
    explicit EventJournal(std::shared_ptr<IStorageAdapter> adapter);

    /// Добавить запись в журнал
    VoidResult append(
        JournalEntryType type,
        const std::string& payload_json,
        const CorrelationId& correlation_id = CorrelationId{""},
        const StrategyId& strategy_id = StrategyId{""},
        const ConfigHash& config_hash = ConfigHash{""});

    /// Запросить записи по временному диапазону с опциональным фильтром
    Result<std::vector<JournalEntry>> query(
        Timestamp from, Timestamp to,
        std::optional<JournalEntryType> type_filter = std::nullopt);

    /// Принудительный сброс буферов
    VoidResult flush();

private:
    std::shared_ptr<IStorageAdapter> adapter_;
    std::atomic<uint64_t> sequence_counter_{0};
    std::mutex mutex_;
};

} // namespace tb::persistence
