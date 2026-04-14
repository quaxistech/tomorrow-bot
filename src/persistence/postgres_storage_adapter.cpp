/**
 * @file postgres_storage_adapter.cpp
 * @brief Реализация PostgreSQL-адаптера хранилища
 */
#include "persistence/postgres_storage_adapter.hpp"
#include "persistence/persistence_types.hpp"
#include "common/errors.hpp"
#include <pqxx/pqxx>
#include <sstream>
#include <stdexcept>

namespace tb::persistence {

// ==================== DDL ====================

static constexpr const char* kCreateJournalTable = R"sql(
CREATE TABLE IF NOT EXISTS tb_journal (
    id            BIGSERIAL PRIMARY KEY,
    sequence_id   BIGINT NOT NULL,
    entry_type    TEXT NOT NULL,
    ts_ns         BIGINT NOT NULL,
    correlation_id TEXT,
    strategy_id   TEXT,
    config_hash   TEXT,
    payload_json  TEXT NOT NULL,
    inserted_at   TIMESTAMPTZ DEFAULT now()
);
CREATE INDEX IF NOT EXISTS tb_journal_ts_idx ON tb_journal(ts_ns);
CREATE INDEX IF NOT EXISTS tb_journal_type_idx ON tb_journal(entry_type);
CREATE INDEX IF NOT EXISTS tb_journal_strategy_idx ON tb_journal(strategy_id);
)sql";

static constexpr const char* kCreateSnapshotsTable = R"sql(
CREATE TABLE IF NOT EXISTS tb_snapshots (
    snapshot_id   BIGSERIAL PRIMARY KEY,
    snap_type     TEXT NOT NULL,
    created_at_ns BIGINT NOT NULL,
    config_hash   TEXT,
    payload_json  TEXT NOT NULL,
    inserted_at   TIMESTAMPTZ DEFAULT now()
);
CREATE INDEX IF NOT EXISTS tb_snapshots_type_idx ON tb_snapshots(snap_type);
)sql";

// ==================== Helpers ====================

namespace {

std::string journal_type_str(JournalEntryType t) {
    return to_string(t);
}

JournalEntryType journal_type_from_str(const std::string& s) {
    if (s == "MarketEvent")       return JournalEntryType::MarketEvent;
    if (s == "DecisionTrace")     return JournalEntryType::DecisionTrace;
    if (s == "RiskDecision")      return JournalEntryType::RiskDecision;
    if (s == "OrderEvent")        return JournalEntryType::OrderEvent;
    if (s == "PortfolioChange")   return JournalEntryType::PortfolioChange;
    if (s == "StrategySignal")    return JournalEntryType::StrategySignal;
    if (s == "SystemEvent")       return JournalEntryType::SystemEvent;
    if (s == "TelemetrySnapshot") return JournalEntryType::TelemetrySnapshot;
    if (s == "GovernanceEvent")   return JournalEntryType::GovernanceEvent;
    if (s == "DiagnosticEvent")   return JournalEntryType::DiagnosticEvent;
    throw std::invalid_argument("Unknown JournalEntryType: " + s);
}

SnapshotType snapshot_type_from_str(const std::string& s) {
    if (s == "Portfolio")       return SnapshotType::Portfolio;
    if (s == "RiskCounters")    return SnapshotType::RiskCounters;
    if (s == "StrategyMeta")    return SnapshotType::StrategyMeta;
    if (s == "WorldState")      return SnapshotType::WorldState;
    if (s == "FullSystem")      return SnapshotType::FullSystem;
    if (s == "GovernanceState") return SnapshotType::GovernanceState;
    throw std::invalid_argument("Unknown SnapshotType: " + s);
}

} // anonymous namespace

// ==================== Constructor / Destructor ====================

PostgresStorageAdapter::PostgresStorageAdapter(std::string connection_string)
    : conn_string_(std::move(connection_string))
{
    conn_ = std::make_unique<pqxx::connection>(conn_string_);
    ensure_schema();
}

PostgresStorageAdapter::~PostgresStorageAdapter() = default;

// ==================== ensure_schema ====================

void PostgresStorageAdapter::ensure_schema() {
    pqxx::work txn(*conn_);
    txn.exec(kCreateJournalTable);
    txn.exec(kCreateSnapshotsTable);
    txn.commit();

    // Инициализируем next_seq_ из максимального sequence_id в таблице
    pqxx::nontransaction ntxn(*conn_);
    auto r = ntxn.exec("SELECT COALESCE(MAX(sequence_id), 0) FROM tb_journal");
    if (!r.empty() && !r[0][0].is_null()) {
        next_seq_ = r[0][0].as<uint64_t>() + 1;
    }
}

// ==================== reconnect_if_needed ====================

void PostgresStorageAdapter::reconnect_if_needed() {
    if (!conn_ || !conn_->is_open()) {
        conn_ = std::make_unique<pqxx::connection>(conn_string_);
        ensure_schema();
    }
}

bool PostgresStorageAdapter::is_connected() const noexcept {
    return conn_ && conn_->is_open();
}

// ==================== append_journal ====================

VoidResult PostgresStorageAdapter::append_journal(const JournalEntry& entry) {
    std::lock_guard lock(mutex_);
    try {
        reconnect_if_needed();
        pqxx::work txn(*conn_);

        // ИСПРАВЛЕНИЕ: Инкремент next_seq_ ПОСЛЕ успешного коммита, не до
        uint64_t seq = next_seq_;  // Читаем текущее значение
        txn.exec_params(
            R"sql(
            INSERT INTO tb_journal
                (sequence_id, entry_type, ts_ns, correlation_id,
                 strategy_id, config_hash, payload_json)
            VALUES ($1, $2, $3, $4, $5, $6, $7)
            )sql",
            static_cast<int64_t>(seq),
            journal_type_str(entry.type),
            static_cast<int64_t>(entry.timestamp.get()),
            entry.correlation_id.get(),
            entry.strategy_id.get(),
            entry.config_hash.get(),
            entry.payload_json
        );
        txn.commit();

        // Инкремент только после успешного коммита
        next_seq_++;

        return OkVoid();
    } catch (const std::exception& e) {
        return ErrVoid(TbError::PersistenceError);
    }
}

// ==================== query_journal ====================

Result<std::vector<JournalEntry>> PostgresStorageAdapter::query_journal(
    Timestamp from, Timestamp to,
    std::optional<JournalEntryType> type_filter)
{
    std::lock_guard lock(mutex_);
    try {
        reconnect_if_needed();
        pqxx::nontransaction ntxn(*conn_);

        pqxx::result rows;
        if (type_filter.has_value()) {
            rows = ntxn.exec_params(
                R"sql(
                SELECT sequence_id, entry_type, ts_ns,
                       correlation_id, strategy_id, config_hash, payload_json
                FROM tb_journal
                WHERE ts_ns >= $1 AND ts_ns <= $2 AND entry_type = $3
                ORDER BY sequence_id ASC
                )sql",
                static_cast<int64_t>(from.get()),
                static_cast<int64_t>(to.get()),
                journal_type_str(*type_filter)
            );
        } else {
            rows = ntxn.exec_params(
                R"sql(
                SELECT sequence_id, entry_type, ts_ns,
                       correlation_id, strategy_id, config_hash, payload_json
                FROM tb_journal
                WHERE ts_ns >= $1 AND ts_ns <= $2
                ORDER BY sequence_id ASC
                )sql",
                static_cast<int64_t>(from.get()),
                static_cast<int64_t>(to.get())
            );
        }

        std::vector<JournalEntry> entries;
        entries.reserve(rows.size());
        for (const auto& row : rows) {
            JournalEntry e;
            e.sequence_id     = static_cast<uint64_t>(row[0].as<int64_t>());
            e.type            = journal_type_from_str(row[1].as<std::string>());
            e.timestamp       = Timestamp{row[2].as<int64_t>()};
            e.correlation_id  = CorrelationId{row[3].is_null() ? "" : row[3].as<std::string>()};
            e.strategy_id     = StrategyId{row[4].is_null() ? "" : row[4].as<std::string>()};
            e.config_hash     = ConfigHash{row[5].is_null() ? "" : row[5].as<std::string>()};
            e.payload_json    = row[6].as<std::string>();
            entries.push_back(std::move(e));
        }
        return Ok(std::move(entries));
    } catch (const std::exception&) {
        return Err<std::vector<JournalEntry>>(TbError::PersistenceError);
    }
}

// ==================== store_snapshot ====================

VoidResult PostgresStorageAdapter::store_snapshot(const SnapshotEntry& entry) {
    std::lock_guard lock(mutex_);
    try {
        reconnect_if_needed();
        pqxx::work txn(*conn_);

        txn.exec_params(
            R"sql(
            INSERT INTO tb_snapshots
                (snap_type, created_at_ns, config_hash, payload_json)
            VALUES ($1, $2, $3, $4)
            )sql",
            to_string(entry.type),
            static_cast<int64_t>(entry.created_at.get()),
            entry.config_hash.get(),
            entry.payload_json
        );
        txn.commit();
        return OkVoid();
    } catch (const std::exception&) {
        return ErrVoid(TbError::PersistenceError);
    }
}

// ==================== load_latest_snapshot ====================

Result<SnapshotEntry> PostgresStorageAdapter::load_latest_snapshot(SnapshotType type) {
    std::lock_guard lock(mutex_);
    try {
        reconnect_if_needed();
        pqxx::nontransaction ntxn(*conn_);

        auto rows = ntxn.exec_params(
            R"sql(
            SELECT snapshot_id, snap_type, created_at_ns, config_hash, payload_json
            FROM tb_snapshots
            WHERE snap_type = $1
            ORDER BY snapshot_id DESC
            LIMIT 1
            )sql",
            to_string(type)
        );

        if (rows.empty()) {
            return Err<SnapshotEntry>(TbError::PersistenceError);
        }

        const auto& row = rows[0];
        SnapshotEntry e;
        e.snapshot_id  = static_cast<uint64_t>(row[0].as<int64_t>());
        e.type         = snapshot_type_from_str(row[1].as<std::string>());
        e.created_at   = Timestamp{row[2].as<int64_t>()};
        e.config_hash  = ConfigHash{row[3].is_null() ? "" : row[3].as<std::string>()};
        e.payload_json = row[4].as<std::string>();
        return Ok(std::move(e));
    } catch (const std::exception&) {
        return Err<SnapshotEntry>(TbError::PersistenceError);
    }
}

// ==================== flush ====================

VoidResult PostgresStorageAdapter::flush() {
    // PostgreSQL автокоммитит в конце каждой транзакции — дополнительный flush не нужен.
    // Метод оставлен для совместимости с интерфейсом (например, файловые адаптеры).
    return OkVoid();
}

// ==================== factory ====================

std::shared_ptr<PostgresStorageAdapter> make_postgres_adapter(
    const std::string& connection_string)
{
    return std::make_shared<PostgresStorageAdapter>(connection_string);
}

} // namespace tb::persistence
