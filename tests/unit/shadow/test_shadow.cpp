/**
 * @file test_shadow.cpp
 * @brief Тесты теневого режима
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "shadow/shadow_mode_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

using namespace tb;
using namespace tb::shadow;
using namespace Catch::Matchers;

// Интервалы отслеживания (наносекунды)
static constexpr int64_t kOneSecNs   = 1'000'000'000LL;
static constexpr int64_t kFiveSecNs  = 5'000'000'000LL;
static constexpr int64_t kThirtySecNs = 30'000'000'000LL;

/// Тестовый логгер — no-op
class ShadowTestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

/// Тестовые часы — no-op
class ShadowTestClock : public clock::IClock {
public:
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
};

/// Создать тестовый движок
static ShadowModeEngine make_engine(ShadowConfig cfg) {
    return ShadowModeEngine(
        std::move(cfg),
        std::make_shared<ShadowTestLogger>(),
        std::make_shared<ShadowTestClock>());
}

/// Вспомогательная функция — создать теневое решение
static ShadowDecision make_decision(
    const std::string& strategy = "momentum",
    const std::string& sym = "BTCUSDT",
    double price = 50000.0,
    int64_t ts = 1000)
{
    ShadowDecision d;
    d.correlation_id = CorrelationId("corr-1");
    d.strategy_id = StrategyId(strategy);
    d.symbol = Symbol(sym);
    d.side = Side::Buy;
    d.quantity = Quantity(0.1);
    d.intended_price = Price(price);
    d.conviction = 0.85;
    d.decided_at = Timestamp(ts);
    d.world_state = "Stable";
    d.regime = "Trending";
    d.uncertainty_level = "Low";
    d.risk_verdict = "Approved";
    d.would_have_been_live = true;
    return d;
}

TEST_CASE("Shadow: Теневое решение записывается корректно", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    auto decision = make_decision();
    auto result = engine.record_decision(decision);
    REQUIRE(result.has_value());

    auto trades = engine.get_trades(StrategyId("momentum"));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].decision.symbol.get() == "BTCUSDT");
    CHECK(trades[0].decision.conviction == 0.85);
    CHECK(trades[0].market_price_at_decision.get() == 50000.0);
    CHECK_FALSE(trades[0].tracking_complete);

    // Fill simulation должна быть заполнена
    CHECK(trades[0].fill_sim.order_state == ShadowOrderState::Filled);
    CHECK(trades[0].fill_sim.simulated_fill_price.get() == 50000.0);

    REQUIRE(engine.get_trade_count(StrategyId("momentum")) == 1);
    REQUIRE(engine.get_trade_count(StrategyId("other")) == 0);
}

TEST_CASE("Shadow: Отслеживание цены обновляется", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    int64_t base_ts = 100'000'000'000LL;
    auto decision = make_decision("momentum", "BTCUSDT", 50000.0, base_ts);
    engine.record_decision(decision);

    // Обновление через 1 секунду
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50100.0),
        Timestamp(base_ts + kOneSecNs));

    auto trades = engine.get_trades(StrategyId("momentum"));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price_tracking.price_at_short.value().get() == 50100.0);
    CHECK_FALSE(trades[0].price_tracking.price_at_mid.has_value());
    CHECK_FALSE(trades[0].tracking_complete);

    // Обновление через 5 секунд
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50200.0),
        Timestamp(base_ts + kFiveSecNs));

    trades = engine.get_trades(StrategyId("momentum"));
    CHECK(trades[0].price_tracking.price_at_mid.value().get() == 50200.0);
    CHECK_FALSE(trades[0].tracking_complete);

    // Обновление через 30 секунд — завершает отслеживание
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50500.0),
        Timestamp(base_ts + kThirtySecNs));

    trades = engine.get_trades(StrategyId("momentum"));
    CHECK(trades[0].price_tracking.price_at_long.value().get() == 50500.0);
    CHECK(trades[0].tracking_complete);

    // P&L: Buy @ 50000, exit @ 50500 → +100 bps (gross)
    CHECK_THAT(trades[0].gross_pnl_bps, WithinAbs(100.0, 1.0));
}

TEST_CASE("Shadow: Сравнение shadow vs live вычисляется", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    int64_t base_ts = 100'000'000'000LL;

    // Записываем 3 теневых решения
    for (int i = 0; i < 3; ++i) {
        auto d = make_decision("strat_a", "BTCUSDT", 50000.0, base_ts + i * kThirtySecNs * 2);
        engine.record_decision(d);

        // Завершаем отслеживание: цена растёт на 0.5% (+50 bps)
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kOneSecNs));
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kFiveSecNs));
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kThirtySecNs));
    }

    auto comparison = engine.compare(StrategyId("strat_a"), 5, 120.0, 0.6);

    CHECK(comparison.shadow_trades == 3);
    CHECK(comparison.live_trades == 5);
    CHECK(comparison.live_pnl_bps == 120.0);
    CHECK(comparison.live_hit_rate == 0.6);
    // 3 прибыльных из 3 → hit_rate = 1.0
    CHECK_THAT(comparison.shadow_hit_rate, WithinAbs(1.0, 0.01));
    // 3 × 50 bps = 150 bps
    CHECK_THAT(comparison.shadow_gross_pnl_bps, WithinAbs(150.0, 5.0));
}

TEST_CASE("Shadow: Лимит записей соблюдается", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 5};
    auto engine = make_engine(cfg);

    // Записываем 10 решений
    for (int i = 0; i < 10; ++i) {
        auto d = make_decision("strat_b", "BTCUSDT", 50000.0 + i, static_cast<int64_t>(i));
        engine.record_decision(d);
    }

    // Должно остаться только 5 (последних)
    auto trades = engine.get_trades(StrategyId("strat_b"));
    REQUIRE(trades.size() == 5);
    // Первая запись должна быть с ценой 50005 (индексы 5..9)
    CHECK(trades[0].decision.intended_price.get() == 50005.0);
    CHECK(engine.get_trade_count(StrategyId("strat_b")) == 5);
}

TEST_CASE("Shadow: Отключённый теневой режим не записывает", "[shadow]") {
    ShadowConfig cfg{.enabled = false, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    REQUIRE_FALSE(engine.is_enabled());

    auto result = engine.record_decision(make_decision());
    // Disabled → returns ShadowDisabled error
    REQUIRE_FALSE(result.has_value());

    auto trades = engine.get_trades(StrategyId("momentum"));
    CHECK(trades.empty());
    CHECK(engine.get_trade_count(StrategyId("momentum")) == 0);
}

TEST_CASE("Shadow: Несовпадающий символ не обновляет цену", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    int64_t base_ts = 100'000'000'000LL;
    auto decision = make_decision("momentum", "BTCUSDT", 50000.0, base_ts);
    engine.record_decision(decision);

    // Обновляем другой символ
    engine.update_price_tracking(Symbol("ETHUSDT"), Price(3000.0),
        Timestamp(base_ts + kOneSecNs));

    auto trades = engine.get_trades(StrategyId("momentum"));
    CHECK_FALSE(trades[0].price_tracking.price_at_short.has_value());
}

TEST_CASE("Shadow: Kill switch блокирует запись", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    engine.set_kill_switch(true);
    auto result = engine.record_decision(make_decision());
    REQUIRE_FALSE(result.has_value());
    CHECK(engine.get_trade_count(StrategyId("momentum")) == 0);

    engine.set_kill_switch(false);
    result = engine.record_decision(make_decision());
    REQUIRE(result.has_value());
    CHECK(engine.get_trade_count(StrategyId("momentum")) == 1);
}

TEST_CASE("Shadow: get_metrics_summary агрегирует статистику", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100};
    auto engine = make_engine(cfg);

    int64_t base_ts = 100'000'000'000LL;

    // 2 завершённые записи
    for (int i = 0; i < 2; ++i) {
        auto d = make_decision("strat_c", "BTCUSDT", 50000.0, base_ts + i * kThirtySecNs * 2);
        engine.record_decision(d);
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kOneSecNs));
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kFiveSecNs));
        engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
            Timestamp(base_ts + i * kThirtySecNs * 2 + kThirtySecNs));
    }

    // 1 незавершённая
    auto d = make_decision("strat_c", "BTCUSDT", 50000.0, base_ts + 4 * kThirtySecNs);
    engine.record_decision(d);

    auto summary = engine.get_metrics_summary();
    CHECK(summary.total_decisions == 3);
    CHECK(summary.completed_decisions == 2);
    CHECK(summary.incomplete_decisions == 1);
    CHECK(summary.hit_rate > 0.0);
}

TEST_CASE("Shadow: check_alerts обнаруживает дивергенцию", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_records_per_strategy = 100,
                     .alert_pnl_divergence_bps = 50.0, .alert_hit_rate_divergence = 0.1};
    auto engine = make_engine(cfg);

    int64_t base_ts = 100'000'000'000LL;
    auto d = make_decision("strat_d", "BTCUSDT", 50000.0, base_ts);
    engine.record_decision(d);
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
        Timestamp(base_ts + kOneSecNs));
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
        Timestamp(base_ts + kFiveSecNs));
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50250.0),
        Timestamp(base_ts + kThirtySecNs));

    // Live P&L очень далеко от shadow
    auto alerts = engine.check_alerts(StrategyId("strat_d"), 1, -500.0, 0.0);
    CHECK_FALSE(alerts.empty());
}
