#pragma once
/**
 * @file postgres_storage_adapter.hpp
 * @brief PostgreSQL-адаптер хранилища через libpqxx 7.x
 *
 * Реализует IStorageAdapter на базе PostgreSQL, обеспечивая надёжную
 * персистентность журнала событий (append-only) и снимков состояния.
 *
 * Схема БД создаётся автоматически при первом подключении:
 *   - tb_journal   — журнал событий
 *   - tb_snapshots — снимки состояния (latest per type)
 *
 * Поддерживается пул соединений размером 1 (single-connection, thread-safe через mutex).
 * Для масштабирования на несколько потоков — передать разные адаптеры.
 */
#include "persistence/storage_adapter.hpp"
#include <string>
#include <mutex>
#include <memory>

// Forward declaration чтобы не тянуть pqxx.hxx в заголовки всей системы
namespace pqxx { class connection; }

namespace tb::persistence {

/// PostgreSQL-адаптер хранилища
class PostgresStorageAdapter : public IStorageAdapter {
public:
    /// @param connection_string  например "host=localhost dbname=tomorrow_bot user=tb"
    explicit PostgresStorageAdapter(std::string connection_string);
    ~PostgresStorageAdapter() override;

    // Некопируемый, неперемещаемый (использовать через shared_ptr)
    PostgresStorageAdapter(const PostgresStorageAdapter&) = delete;
    PostgresStorageAdapter& operator=(const PostgresStorageAdapter&) = delete;
    PostgresStorageAdapter(PostgresStorageAdapter&&) = delete;
    PostgresStorageAdapter& operator=(PostgresStorageAdapter&&) = delete;

    VoidResult append_journal(const JournalEntry& entry) override;

    Result<std::vector<JournalEntry>> query_journal(
        Timestamp from, Timestamp to,
        std::optional<JournalEntryType> type_filter = std::nullopt) override;

    VoidResult store_snapshot(const SnapshotEntry& entry) override;

    Result<SnapshotEntry> load_latest_snapshot(SnapshotType type) override;

    VoidResult flush() override;

    /// Проверить доступность соединения
    [[nodiscard]] bool is_connected() const noexcept;

private:
    void ensure_schema();
    void reconnect_if_needed();

    std::string conn_string_;
    std::unique_ptr<pqxx::connection> conn_;
    mutable std::mutex mutex_;
};

/// Фабричная функция — создаёт адаптер или выбрасывает исключение при ошибке
std::shared_ptr<PostgresStorageAdapter> make_postgres_adapter(
    const std::string& connection_string);

} // namespace tb::persistence
