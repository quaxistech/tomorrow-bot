/**
 * @file test_replay.cpp
 * @brief Тесты движка воспроизведения
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "replay/replay_engine.hpp"
#include "persistence/memory_storage_adapter.hpp"

using namespace tb;
using namespace tb::replay;
using namespace tb::persistence;

// Заполняет адаптер тестовыми данными
inline std::shared_ptr<MemoryStorageAdapter> make_populated_adapter(int count = 5) {
    auto adapter = std::make_shared<MemoryStorageAdapter>();
    for (int i = 0; i < count; ++i) {
        JournalEntry entry;
        entry.sequence_id = static_cast<uint64_t>(i + 1);
        entry.type = (i % 2 == 0) ? JournalEntryType::MarketEvent : JournalEntryType::DecisionTrace;
        entry.timestamp = Timestamp{static_cast<int64_t>((i + 1) * 100)};
        entry.strategy_id = StrategyId{"strat-A"};
        entry.payload_json = R"({"idx":)" + std::to_string(i) + "}";
        adapter->append_journal(entry);
    }
    return adapter;
}

TEST_CASE("ReplayEngine: начальное состояние Idle", "[replay]") {
    auto adapter = make_populated_adapter();
    ReplayEngine engine(adapter);

    REQUIRE(engine.get_state() == ReplayState::Idle);
    REQUIRE(!engine.has_next());
}

TEST_CASE("ReplayEngine: конфигурация и запуск", "[replay]") {
    auto adapter = make_populated_adapter();
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};

    auto cfg_result = engine.configure(config);
    REQUIRE(cfg_result.has_value());

    auto start_result = engine.start();
    REQUIRE(start_result.has_value());
    REQUIRE(engine.get_state() == ReplayState::Playing);
    REQUIRE(engine.has_next());
}

TEST_CASE("ReplayEngine: пошаговое воспроизведение всех событий", "[replay]") {
    auto adapter = make_populated_adapter(3);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    int count = 0;
    while (engine.has_next()) {
        auto event = engine.step();
        REQUIRE(event.has_value());
        ++count;
    }
    REQUIRE(count == 3);
    REQUIRE(engine.get_state() == ReplayState::Completed);
}

TEST_CASE("ReplayEngine: реконструкция решений", "[replay]") {
    auto adapter = make_populated_adapter(5);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.reconstruct_decisions = true;
    engine.configure(config);
    engine.start();

    int reconstructed = 0;
    while (engine.has_next()) {
        auto event = engine.step();
        REQUIRE(event.has_value());
        if (event->was_reconstructed) ++reconstructed;
    }
    // Индексы 1, 3 — DecisionTrace (нечётные)
    REQUIRE(reconstructed == 2);

    auto result = engine.get_result();
    REQUIRE(result.decisions_reconstructed == 2);
    REQUIRE(result.events_replayed == 5);
    REQUIRE(result.final_state == ReplayState::Completed);
}

TEST_CASE("ReplayEngine: ошибка при step без start", "[replay]") {
    auto adapter = make_populated_adapter();
    ReplayEngine engine(adapter);

    auto event = engine.step();
    REQUIRE(!event.has_value());
}

TEST_CASE("ReplayEngine: сброс состояния", "[replay]") {
    auto adapter = make_populated_adapter();
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    engine.step();
    engine.reset();

    REQUIRE(engine.get_state() == ReplayState::Idle);
    REQUIRE(!engine.has_next());
}

TEST_CASE("ReplayEngine: результат содержит предупреждение при пустом диапазоне", "[replay]") {
    auto adapter = make_populated_adapter(3);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{9000};
    config.end_time = Timestamp{9999};
    engine.configure(config);
    engine.start();

    // Нет событий — сразу Completed
    while (engine.has_next()) { engine.step(); }
    auto result = engine.get_result();
    REQUIRE(result.events_replayed == 0);
    REQUIRE(!result.warnings.empty());
}
