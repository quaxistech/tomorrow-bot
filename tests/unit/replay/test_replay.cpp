/**
 * @file test_replay.cpp
 * @brief Тесты движка воспроизведения и бэктест-движка
 *
 * Покрывает: ReplayEngine (базовый прогон, pause/resume/seek,
 * режимы воспроизведения, callbacks, классификация событий),
 * FillSimulator (исполнение, проскальзывание, комиссии),
 * BacktestEngine (прогон, метрики, кривая капитала).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "replay/replay_engine.hpp"
#include "replay/backtest_engine.hpp"
#include "replay/fill_simulator.hpp"
#include "persistence/memory_storage_adapter.hpp"

using namespace tb;
using namespace tb::replay;
using namespace tb::persistence;
using Catch::Approx;

// ============================================================
// Утилиты для генерации тестовых данных
// ============================================================

/// Заполняет адаптер тестовыми событиями разных типов
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

/// Заполняет адаптер событиями разных типов для классификации
inline std::shared_ptr<MemoryStorageAdapter> make_mixed_adapter() {
    auto adapter = std::make_shared<MemoryStorageAdapter>();
    int seq = 1;
    auto add = [&](JournalEntryType type, int64_t ts) {
        JournalEntry entry;
        entry.sequence_id = static_cast<uint64_t>(seq++);
        entry.type = type;
        entry.timestamp = Timestamp{ts};
        entry.strategy_id = StrategyId{"strat-A"};
        entry.payload_json = "{}";
        adapter->append_journal(entry);
    };

    add(JournalEntryType::MarketEvent, 100);
    add(JournalEntryType::DecisionTrace, 200);
    add(JournalEntryType::RiskDecision, 300);
    add(JournalEntryType::OrderEvent, 400);
    add(JournalEntryType::PortfolioChange, 500);
    add(JournalEntryType::SystemEvent, 600);
    add(JournalEntryType::StrategySignal, 700);
    add(JournalEntryType::TelemetrySnapshot, 800);

    return adapter;
}

// ============================================================
// ReplayEngine: базовые тесты (обратная совместимость)
// ============================================================

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

// ============================================================
// ReplayEngine: pause/resume
// ============================================================

TEST_CASE("ReplayEngine: pause и resume", "[replay]") {
    auto adapter = make_populated_adapter(5);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    // Сделать 2 шага
    engine.step();
    engine.step();
    REQUIRE(engine.current_index() == 2);

    // Пауза
    auto pause_result = engine.pause();
    REQUIRE(pause_result.has_value());
    REQUIRE(engine.get_state() == ReplayState::Paused);

    // step() не работает в Paused
    auto step_in_pause = engine.step();
    REQUIRE(!step_in_pause.has_value());

    // Resume
    auto resume_result = engine.resume();
    REQUIRE(resume_result.has_value());
    REQUIRE(engine.get_state() == ReplayState::Playing);

    // step() снова работает
    auto step_after = engine.step();
    REQUIRE(step_after.has_value());
    REQUIRE(engine.current_index() == 3);
}

TEST_CASE("ReplayEngine: pause в неправильном состоянии", "[replay]") {
    auto adapter = make_populated_adapter();
    ReplayEngine engine(adapter);

    // Pause из Idle — ошибка
    REQUIRE(!engine.pause().has_value());

    // Resume из Idle — ошибка
    REQUIRE(!engine.resume().has_value());
}

// ============================================================
// ReplayEngine: seek
// ============================================================

TEST_CASE("ReplayEngine: seek по индексу", "[replay]") {
    auto adapter = make_populated_adapter(10);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{2000};
    engine.configure(config);
    engine.start();

    // Seek к 5-му событию
    auto seek_result = engine.seek(5);
    REQUIRE(seek_result.has_value());
    REQUIRE(engine.current_index() == 5);

    // Step — получаем 6-е событие (index 5)
    auto event = engine.step();
    REQUIRE(event.has_value());
    REQUIRE(event->sequence_index == 5);
}

TEST_CASE("ReplayEngine: seek за пределы буфера", "[replay]") {
    auto adapter = make_populated_adapter(5);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    REQUIRE(!engine.seek(100).has_value());
}

TEST_CASE("ReplayEngine: seek_to_time", "[replay]") {
    auto adapter = make_populated_adapter(10);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{2000};
    engine.configure(config);
    engine.start();

    // Seek к timestamp >= 500 (событие с ts=500 имеет index=4)
    auto seek_result = engine.seek_to_time(Timestamp{500});
    REQUIRE(seek_result.has_value());

    auto event = engine.step();
    REQUIRE(event.has_value());
    REQUIRE(event->journal_entry.timestamp.get() >= 500);
}

TEST_CASE("ReplayEngine: seek_to_time восстанавливает Playing после Completed", "[replay]") {
    auto adapter = make_populated_adapter(5);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    // Прогнать до конца
    while (engine.has_next()) engine.step();
    REQUIRE(engine.get_state() == ReplayState::Completed);

    // Seek назад
    auto seek_result = engine.seek(0);
    REQUIRE(seek_result.has_value());
    REQUIRE(engine.get_state() == ReplayState::Playing);
    REQUIRE(engine.has_next());
}

// ============================================================
// ReplayEngine: progress и total_events
// ============================================================

TEST_CASE("ReplayEngine: progress и total_events", "[replay]") {
    auto adapter = make_populated_adapter(4);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    REQUIRE(engine.total_events() == 4);
    REQUIRE(engine.progress() == Approx(0.0));

    engine.step();
    engine.step();
    REQUIRE(engine.progress() == Approx(0.5));

    engine.step();
    engine.step();
    REQUIRE(engine.progress() == Approx(1.0));
}

// ============================================================
// ReplayEngine: event callback
// ============================================================

TEST_CASE("ReplayEngine: on_event callback вызывается", "[replay]") {
    auto adapter = make_populated_adapter(3);
    ReplayEngine engine(adapter);

    int callback_count = 0;
    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.on_event = [&callback_count](const ReplayEvent&) {
        ++callback_count;
    };

    engine.configure(config);
    engine.start();

    while (engine.has_next()) engine.step();
    REQUIRE(callback_count == 3);
}

// ============================================================
// ReplayEngine: max_events limit
// ============================================================

TEST_CASE("ReplayEngine: max_events ограничивает количество", "[replay]") {
    auto adapter = make_populated_adapter(10);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{2000};
    config.max_events = 3;

    engine.configure(config);
    engine.start();

    REQUIRE(engine.total_events() == 3);

    int count = 0;
    while (engine.has_next()) {
        engine.step();
        ++count;
    }
    REQUIRE(count == 3);
}

// ============================================================
// ReplayEngine: классификация событий по типам
// ============================================================

TEST_CASE("ReplayEngine: результат содержит разбивку по типам", "[replay]") {
    auto adapter = make_mixed_adapter();
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{10000};
    engine.configure(config);
    engine.start();

    while (engine.has_next()) engine.step();

    auto result = engine.get_result();
    REQUIRE(result.market_events == 1);
    REQUIRE(result.decision_events == 2);    // DecisionTrace + StrategySignal
    REQUIRE(result.risk_events == 1);
    REQUIRE(result.order_events == 1);
    REQUIRE(result.portfolio_events == 1);
    REQUIRE(result.system_events == 2);      // SystemEvent + TelemetrySnapshot
}

// ============================================================
// ReplayEngine: режимы воспроизведения
// ============================================================

TEST_CASE("ReplayEngine: режим Inspection — минимальное обогащение", "[replay]") {
    auto adapter = make_mixed_adapter();
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{10000};
    config.mode = ReplayMode::Inspection;
    engine.configure(config);
    engine.start();

    while (engine.has_next()) {
        auto event = engine.step();
        REQUIRE(event.has_value());
        // В Inspection — контексты не заполняются
        REQUIRE(!event->decision.valid);
        REQUIRE(!event->order.valid);
        REQUIRE(!event->risk.valid);
        REQUIRE(!event->portfolio.valid);
    }
}

TEST_CASE("ReplayEngine: режим FullSystem — обогащение всех контекстов", "[replay]") {
    auto adapter = make_mixed_adapter();
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{10000};
    config.mode = ReplayMode::FullSystem;
    engine.configure(config);
    engine.start();

    bool found_decision = false;
    bool found_order = false;
    bool found_risk = false;
    bool found_portfolio = false;

    while (engine.has_next()) {
        auto event = engine.step();
        REQUIRE(event.has_value());
        if (event->decision.valid) found_decision = true;
        if (event->order.valid) found_order = true;
        if (event->risk.valid) found_risk = true;
        if (event->portfolio.valid) found_portfolio = true;
    }

    REQUIRE(found_decision);
    REQUIRE(found_order);
    REQUIRE(found_risk);
    REQUIRE(found_portfolio);
}

// ============================================================
// ReplayEngine: simulated_time и speed_factor
// ============================================================

TEST_CASE("ReplayEngine: simulated_time в Accelerated mode", "[replay]") {
    auto adapter = make_populated_adapter(3);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.time_mode = ReplayTimeMode::Accelerated;
    engine.configure(config);
    engine.start();

    auto event = engine.step();
    REQUIRE(event.has_value());
    // В Accelerated — simulated_time == entry timestamp
    REQUIRE(event->simulated_time.get() == event->journal_entry.timestamp.get());
}

TEST_CASE("ReplayEngine: simulated_duration_ns в результате", "[replay]") {
    auto adapter = make_populated_adapter(5);
    ReplayEngine engine(adapter);

    ReplayConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    engine.configure(config);
    engine.start();

    while (engine.has_next()) engine.step();

    auto result = engine.get_result();
    // Первое событие ts=100, последнее ts=500, разница = 400
    REQUIRE(result.simulated_duration_ns == 400);
}

// ============================================================
// FillSimulator: тесты
// ============================================================

TEST_CASE("FillSimulator: market buy исполняется по ask", "[replay][fill]") {
    FillSimulator sim(FeeModel{0.001, 0.001, true}, SlippageModel{0.0, 0.0, 0.0, false});

    FillRequest req;
    req.side = Side::Buy;
    req.order_type = OrderType::Market;
    req.quantity = Quantity(1.0);
    req.best_bid = Price(99.0);
    req.best_ask = Price(101.0);
    req.available_depth = 1e9;

    auto result = sim.simulate(req);
    REQUIRE(result.filled);
    REQUIRE(!result.partial);
    REQUIRE(result.fill_price.get() == Approx(101.0));
    REQUIRE(result.fee == Approx(101.0 * 0.001));
}

TEST_CASE("FillSimulator: market sell исполняется по bid", "[replay][fill]") {
    FillSimulator sim(FeeModel{0.001, 0.001, true}, SlippageModel{0.0, 0.0, 0.0, false});

    FillRequest req;
    req.side = Side::Sell;
    req.order_type = OrderType::Market;
    req.quantity = Quantity(1.0);
    req.best_bid = Price(99.0);
    req.best_ask = Price(101.0);
    req.available_depth = 1e9;

    auto result = sim.simulate(req);
    REQUIRE(result.filled);
    REQUIRE(result.fill_price.get() == Approx(99.0));
}

TEST_CASE("FillSimulator: нулевой объём отклоняется", "[replay][fill]") {
    FillSimulator sim;

    FillRequest req;
    req.quantity = Quantity(0.0);
    req.best_bid = Price(100.0);
    req.best_ask = Price(101.0);

    auto result = sim.simulate(req);
    REQUIRE(!result.filled);
    REQUIRE(!result.rejection_reason.empty());
}

TEST_CASE("FillSimulator: проскальзывание увеличивает цену buy", "[replay][fill]") {
    FillSimulator sim(FeeModel{}, SlippageModel{5.0, 0.0, 0.0, true});

    FillRequest req;
    req.side = Side::Buy;
    req.order_type = OrderType::Market;
    req.quantity = Quantity(1.0);
    req.best_bid = Price(100.0);
    req.best_ask = Price(100.0);
    req.available_depth = 1e9;

    auto result = sim.simulate(req);
    REQUIRE(result.filled);
    REQUIRE(result.fill_price.get() > 100.0);
    REQUIRE(result.slippage_bps >= 5.0);
}

TEST_CASE("FillSimulator: частичное исполнение при малой ликвидности", "[replay][fill]") {
    FillSimulator sim(FeeModel{}, SlippageModel{0.0, 0.0, 0.0, false});

    FillRequest req;
    req.side = Side::Buy;
    req.order_type = OrderType::Market;
    req.quantity = Quantity(10.0);
    req.requested_price = Price(100.0);
    req.best_bid = Price(99.0);
    req.best_ask = Price(100.0);
    req.available_depth = 500.0;  // Только на 5 единиц

    auto result = sim.simulate(req);
    REQUIRE(result.filled);
    REQUIRE(result.partial);
    REQUIRE(result.filled_qty.get() < 10.0);
}

TEST_CASE("FillSimulator: limit buy ниже ask не исполняется", "[replay][fill]") {
    FillSimulator sim(FeeModel{}, SlippageModel{0.0, 0.0, 0.0, false});

    FillRequest req;
    req.side = Side::Buy;
    req.order_type = OrderType::Limit;
    req.quantity = Quantity(1.0);
    req.requested_price = Price(98.0);  // Ниже ask=100
    req.best_bid = Price(99.0);
    req.best_ask = Price(100.0);

    auto result = sim.simulate(req);
    REQUIRE(!result.filled);
}

// ============================================================
// BacktestEngine: тесты
// ============================================================

TEST_CASE("BacktestEngine: конфигурация с невалидным капиталом", "[replay][backtest]") {
    auto adapter = make_populated_adapter();
    BacktestEngine engine(adapter);

    BacktestConfig config;
    config.initial_capital = -100.0;

    REQUIRE(!engine.configure(config).has_value());
}

TEST_CASE("BacktestEngine: прогон по пустому диапазону", "[replay][backtest]") {
    auto adapter = make_populated_adapter(3);
    BacktestEngine engine(adapter);

    BacktestConfig config;
    config.start_time = Timestamp{9000};
    config.end_time = Timestamp{9999};
    config.initial_capital = 10000.0;

    engine.configure(config);
    auto result = engine.run();
    REQUIRE(result.has_value());
    REQUIRE(!result->is_valid());  // Нет сделок
    REQUIRE(!result->warnings.empty());
}

TEST_CASE("BacktestEngine: прогон с событиями генерирует replay result", "[replay][backtest]") {
    auto adapter = make_populated_adapter(5);
    BacktestEngine engine(adapter);

    BacktestConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.initial_capital = 10000.0;

    engine.configure(config);
    auto result = engine.run();
    REQUIRE(result.has_value());
    REQUIRE(result->replay_result.events_replayed == 5);
    REQUIRE(result->replay_result.final_state == ReplayState::Completed);
    REQUIRE(!result->equity_curve.empty());
    REQUIRE(!result->run_id.empty());
    REQUIRE(result->backtest_wall_time_ms >= 0);
}

TEST_CASE("BacktestEngine: кривая капитала начинается с initial_capital", "[replay][backtest]") {
    auto adapter = make_populated_adapter(3);
    BacktestEngine engine(adapter);

    BacktestConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.initial_capital = 50000.0;

    engine.configure(config);
    auto result = engine.run();
    REQUIRE(result.has_value());
    REQUIRE(!result->equity_curve.empty());
    REQUIRE(result->equity_curve.front().equity == Approx(50000.0));
}

TEST_CASE("BacktestEngine: reset очищает состояние", "[replay][backtest]") {
    auto adapter = make_populated_adapter(3);
    BacktestEngine engine(adapter);

    BacktestConfig config;
    config.start_time = Timestamp{0};
    config.end_time = Timestamp{1000};
    config.initial_capital = 10000.0;

    engine.configure(config);
    engine.run();

    engine.reset();
    REQUIRE(engine.get_state() == ReplayState::Idle);
    REQUIRE(engine.progress() == Approx(0.0));
}

// ============================================================
// compute_metrics: тесты
// ============================================================

TEST_CASE("compute_metrics: пустой список сделок", "[replay][metrics]") {
    std::vector<TradeRecord> trades;
    std::vector<EquityPoint> equity;
    auto m = compute_metrics(trades, equity, 10000.0, 1000000);
    REQUIRE(m.total_trades == 0);
    REQUIRE(m.total_pnl == Approx(0.0));
}

TEST_CASE("compute_metrics: одна прибыльная сделка", "[replay][metrics]") {
    TradeRecord t;
    t.net_pnl = 100.0;
    t.return_pct = 1.0;
    t.total_fees = 2.0;
    t.slippage_cost = 0.5;
    t.entry_price = Price(100.0);
    t.quantity = Quantity(10.0);
    t.hold_duration_ns = 3600'000'000'000LL;

    std::vector<TradeRecord> trades = {t};

    EquityPoint p1{Timestamp{0}, 10000.0, 9000.0, 1000.0, 0.0, 1};
    EquityPoint p2{Timestamp{100}, 10100.0, 10100.0, 0.0, 0.0, 0};
    std::vector<EquityPoint> equity = {p1, p2};

    auto m = compute_metrics(trades, equity, 10000.0, 3600'000'000'000LL);
    REQUIRE(m.total_trades == 1);
    REQUIRE(m.winning_trades == 1);
    REQUIRE(m.losing_trades == 0);
    REQUIRE(m.win_rate == Approx(100.0));
    REQUIRE(m.total_pnl == Approx(100.0));
    REQUIRE(m.total_return_pct == Approx(1.0));
    REQUIRE(m.total_fees == Approx(2.0));
    REQUIRE(m.max_consecutive_wins == 1);
}

TEST_CASE("compute_metrics: серия wins и losses", "[replay][metrics]") {
    std::vector<TradeRecord> trades;
    // Win, Win, Loss, Win, Loss, Loss
    double pnls[] = {50.0, 30.0, -20.0, 40.0, -15.0, -25.0};
    for (int i = 0; i < 6; ++i) {
        TradeRecord t;
        t.net_pnl = pnls[i];
        t.return_pct = pnls[i] / 100.0;
        t.total_fees = 1.0;
        t.entry_price = Price(100.0);
        t.quantity = Quantity(1.0);
        t.hold_duration_ns = 3600'000'000'000LL;
        trades.push_back(t);
    }

    std::vector<EquityPoint> equity;
    double eq = 10000.0;
    for (int i = 0; i < 6; ++i) {
        eq += pnls[i];
        equity.push_back({Timestamp{static_cast<int64_t>((i + 1) * 100)}, eq, eq, 0.0, 0.0, 0});
    }

    auto m = compute_metrics(trades, equity, 10000.0, 3600'000'000'000LL * 6);
    REQUIRE(m.total_trades == 6);
    REQUIRE(m.winning_trades == 3);
    REQUIRE(m.losing_trades == 3);
    REQUIRE(m.max_consecutive_wins == 2);
    REQUIRE(m.max_consecutive_losses == 2);
    REQUIRE(m.profit_factor > 0.0);
}

