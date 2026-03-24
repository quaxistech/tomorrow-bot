/**
 * @file test_shadow.cpp
 * @brief Тесты теневого режима
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "shadow/shadow_mode_engine.hpp"

using namespace tb;
using namespace tb::shadow;
using namespace Catch::Matchers;

// Интервалы отслеживания (наносекунды)
static constexpr int64_t kOneSecNs   = 1'000'000'000LL;
static constexpr int64_t kFiveSecNs  = 5'000'000'000LL;
static constexpr int64_t kThirtySecNs = 30'000'000'000LL;

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
    ShadowConfig cfg{.enabled = true, .max_shadow_records = 100};
    ShadowModeEngine engine(cfg);

    auto decision = make_decision();
    auto result = engine.record_shadow_decision(decision);
    REQUIRE(result.has_value());

    auto trades = engine.get_shadow_trades(StrategyId("momentum"));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].decision.symbol.get() == "BTCUSDT");
    CHECK(trades[0].decision.conviction == 0.85);
    CHECK(trades[0].market_price_at_decision.get() == 50000.0);
    CHECK_FALSE(trades[0].price_tracking_complete);

    REQUIRE(engine.get_shadow_trades_count(StrategyId("momentum")) == 1);
    REQUIRE(engine.get_shadow_trades_count(StrategyId("other")) == 0);
}

TEST_CASE("Shadow: Отслеживание цены обновляется", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_shadow_records = 100};
    ShadowModeEngine engine(cfg);

    int64_t base_ts = 100'000'000'000LL;
    auto decision = make_decision("momentum", "BTCUSDT", 50000.0, base_ts);
    engine.record_shadow_decision(decision);

    // Обновление через 1 секунду
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50100.0),
        Timestamp(base_ts + kOneSecNs));

    auto trades = engine.get_shadow_trades(StrategyId("momentum"));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].market_price_after_1s.get() == 50100.0);
    CHECK(trades[0].market_price_after_5s.get() == 0.0);
    CHECK_FALSE(trades[0].price_tracking_complete);

    // Обновление через 5 секунд
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50200.0),
        Timestamp(base_ts + kFiveSecNs));

    trades = engine.get_shadow_trades(StrategyId("momentum"));
    CHECK(trades[0].market_price_after_5s.get() == 50200.0);
    CHECK_FALSE(trades[0].price_tracking_complete);

    // Обновление через 30 секунд — завершает отслеживание
    engine.update_price_tracking(Symbol("BTCUSDT"), Price(50500.0),
        Timestamp(base_ts + kThirtySecNs));

    trades = engine.get_shadow_trades(StrategyId("momentum"));
    CHECK(trades[0].market_price_after_30s.get() == 50500.0);
    CHECK(trades[0].price_tracking_complete);

    // P&L: Buy @ 50000, exit @ 50500 → +100 bps
    CHECK_THAT(trades[0].hypothetical_pnl_bps, WithinAbs(100.0, 1.0));
}

TEST_CASE("Shadow: Сравнение shadow vs live вычисляется", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_shadow_records = 100};
    ShadowModeEngine engine(cfg);

    int64_t base_ts = 100'000'000'000LL;

    // Записываем 3 теневых решения
    for (int i = 0; i < 3; ++i) {
        auto d = make_decision("strat_a", "BTCUSDT", 50000.0, base_ts + i * kThirtySecNs * 2);
        engine.record_shadow_decision(d);

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
    CHECK_THAT(comparison.shadow_pnl_bps, WithinAbs(150.0, 5.0));
}

TEST_CASE("Shadow: Лимит записей соблюдается", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_shadow_records = 5};
    ShadowModeEngine engine(cfg);

    // Записываем 10 решений
    for (int i = 0; i < 10; ++i) {
        auto d = make_decision("strat_b", "BTCUSDT", 50000.0 + i, static_cast<int64_t>(i));
        engine.record_shadow_decision(d);
    }

    // Должно остаться только 5 (последних)
    auto trades = engine.get_shadow_trades(StrategyId("strat_b"));
    REQUIRE(trades.size() == 5);
    // Первая запись должна быть с ценой 50005 (индексы 5..9)
    CHECK(trades[0].decision.intended_price.get() == 50005.0);
    CHECK(engine.get_shadow_trades_count(StrategyId("strat_b")) == 5);
}

TEST_CASE("Shadow: Отключённый теневой режим не записывает", "[shadow]") {
    ShadowConfig cfg{.enabled = false, .max_shadow_records = 100};
    ShadowModeEngine engine(cfg);

    REQUIRE_FALSE(engine.is_enabled());

    auto result = engine.record_shadow_decision(make_decision());
    REQUIRE(result.has_value());

    auto trades = engine.get_shadow_trades(StrategyId("momentum"));
    CHECK(trades.empty());
    CHECK(engine.get_shadow_trades_count(StrategyId("momentum")) == 0);
}

TEST_CASE("Shadow: Несовпадающий символ не обновляет цену", "[shadow]") {
    ShadowConfig cfg{.enabled = true, .max_shadow_records = 100};
    ShadowModeEngine engine(cfg);

    int64_t base_ts = 100'000'000'000LL;
    auto decision = make_decision("momentum", "BTCUSDT", 50000.0, base_ts);
    engine.record_shadow_decision(decision);

    // Обновляем другой символ
    engine.update_price_tracking(Symbol("ETHUSDT"), Price(3000.0),
        Timestamp(base_ts + kOneSecNs));

    auto trades = engine.get_shadow_trades(StrategyId("momentum"));
    CHECK(trades[0].market_price_after_1s.get() == 0.0);
}
