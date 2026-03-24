/**
 * @file test_persistence.cpp
 * @brief Тесты слоя персистентности
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "persistence/persistence_layer.hpp"
#include "persistence/memory_storage_adapter.hpp"
#include "persistence/event_journal.hpp"
#include "persistence/snapshot_store.hpp"

using namespace tb;
using namespace tb::persistence;

// Создаёт in-memory адаптер для тестов
inline std::shared_ptr<MemoryStorageAdapter> make_test_adapter() {
    return std::make_shared<MemoryStorageAdapter>();
}

TEST_CASE("MemoryStorageAdapter: добавление и запрос записей журнала", "[persistence]") {
    auto adapter = make_test_adapter();

    JournalEntry entry;
    entry.sequence_id = 1;
    entry.type = JournalEntryType::MarketEvent;
    entry.timestamp = Timestamp{1000};
    entry.correlation_id = CorrelationId{"corr-1"};
    entry.payload_json = R"({"price": 50000})";

    auto result = adapter->append_journal(entry);
    REQUIRE(result.has_value());
    REQUIRE(adapter->journal_size() == 1);

    auto query = adapter->query_journal(Timestamp{0}, Timestamp{2000});
    REQUIRE(query.has_value());
    REQUIRE(query->size() == 1);
    REQUIRE((*query)[0].sequence_id == 1);
    REQUIRE((*query)[0].payload_json == R"({"price": 50000})");
}

TEST_CASE("MemoryStorageAdapter: фильтрация по типу записи", "[persistence]") {
    auto adapter = make_test_adapter();

    JournalEntry e1;
    e1.sequence_id = 1;
    e1.type = JournalEntryType::MarketEvent;
    e1.timestamp = Timestamp{100};

    JournalEntry e2;
    e2.sequence_id = 2;
    e2.type = JournalEntryType::OrderEvent;
    e2.timestamp = Timestamp{200};

    JournalEntry e3;
    e3.sequence_id = 3;
    e3.type = JournalEntryType::MarketEvent;
    e3.timestamp = Timestamp{300};

    adapter->append_journal(e1);
    adapter->append_journal(e2);
    adapter->append_journal(e3);

    auto filtered = adapter->query_journal(
        Timestamp{0}, Timestamp{1000}, JournalEntryType::MarketEvent);
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->size() == 2);
}

TEST_CASE("MemoryStorageAdapter: снимки состояний", "[persistence]") {
    auto adapter = make_test_adapter();

    SnapshotEntry snap;
    snap.snapshot_id = 1;
    snap.type = SnapshotType::Portfolio;
    snap.created_at = Timestamp{500};
    snap.payload_json = R"({"btc": 1.5})";

    auto save_result = adapter->store_snapshot(snap);
    REQUIRE(save_result.has_value());

    auto load_result = adapter->load_latest_snapshot(SnapshotType::Portfolio);
    REQUIRE(load_result.has_value());
    REQUIRE(load_result->snapshot_id == 1);
    REQUIRE(load_result->payload_json == R"({"btc": 1.5})");

    // Загрузка несуществующего типа
    auto missing = adapter->load_latest_snapshot(SnapshotType::RiskCounters);
    REQUIRE(!missing.has_value());
}

TEST_CASE("EventJournal: автоматическая генерация sequence_id", "[persistence]") {
    auto adapter = make_test_adapter();
    EventJournal journal(adapter);

    auto r1 = journal.append(JournalEntryType::MarketEvent, R"({"data":1})");
    REQUIRE(r1.has_value());

    auto r2 = journal.append(JournalEntryType::OrderEvent, R"({"data":2})");
    REQUIRE(r2.has_value());

    auto entries = adapter->query_journal(Timestamp{0}, Timestamp{std::numeric_limits<int64_t>::max()});
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    REQUIRE((*entries)[0].sequence_id == 1);
    REQUIRE((*entries)[1].sequence_id == 2);
}

TEST_CASE("SnapshotStore: сохранение и загрузка снимков", "[persistence]") {
    auto adapter = make_test_adapter();
    SnapshotStore store(adapter);

    auto save1 = store.save(SnapshotType::Portfolio, R"({"v":1})");
    REQUIRE(save1.has_value());

    auto save2 = store.save(SnapshotType::Portfolio, R"({"v":2})");
    REQUIRE(save2.has_value());

    // Последний снимок перезаписывает предыдущий (в in-memory реализации)
    auto loaded = store.load_latest(SnapshotType::Portfolio);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->payload_json == R"({"v":2})");
}

TEST_CASE("PersistenceLayer: фасад работы с журналом и снимками", "[persistence]") {
    auto adapter = make_test_adapter();
    PersistenceLayer layer(adapter);

    REQUIRE(layer.is_enabled());

    // Журнал
    auto r = layer.journal().append(
        JournalEntryType::SystemEvent,
        R"({"msg":"startup"})",
        CorrelationId{"c-1"},
        StrategyId{"strat-1"});
    REQUIRE(r.has_value());

    // Снимки
    auto s = layer.snapshots().save(SnapshotType::FullSystem, R"({"state":"ok"})");
    REQUIRE(s.has_value());

    auto loaded = layer.snapshots().load_latest(SnapshotType::FullSystem);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->payload_json == R"({"state":"ok"})");

    // Flush
    auto f = layer.flush();
    REQUIRE(f.has_value());
}

TEST_CASE("PersistenceLayer: конфигурация отключённой персистентности", "[persistence]") {
    auto adapter = make_test_adapter();
    PersistenceConfig config;
    config.enabled = false;
    PersistenceLayer layer(adapter, config);

    REQUIRE(!layer.is_enabled());
}
