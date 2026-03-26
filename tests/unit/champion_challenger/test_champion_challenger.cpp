/**
 * @file test_champion_challenger.cpp
 * @brief Тесты Champion-Challenger A/B тестирования (v2)
 *
 * Покрываемые функциональности:
 *  - Регистрация и повторная регистрация
 *  - Запись результатов с PnLBreakdown (net P&L = gross - fee - slippage)
 *  - Drawdown tracking
 *  - Pre-promotion audit (hit_rate, drawdown, regime consistency)
 *  - Промоушен / отклонение
 *  - Observer callbacks
 *  - Граничные случаи: недостаточно данных, незарегистрированная стратегия
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "champion_challenger/champion_challenger_engine.hpp"

using namespace tb;
using namespace tb::champion_challenger;
using namespace Catch::Matchers;

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------

/// Сокращённый конструктор PnLBreakdown (только gross, без издержек)
static PnLBreakdown simple_pnl(double gross_bps) {
    return {.gross_pnl_bps = gross_bps, .fee_bps = 0.0, .slippage_bps = 0.0};
}

/// Конфигурация по умолчанию для тестов (мало данных для быстрой проверки)
static ChampionChallengerConfig default_config() {
    ChampionChallengerConfig cfg;
    cfg.min_evaluation_trades = 3;
    cfg.promotion_threshold   = 0.2;
    cfg.rejection_threshold   = -0.1;
    cfg.min_hit_rate          = 0.45;
    cfg.max_drawdown_bps      = -500.0;
    cfg.min_regime_samples    = 2;
    return cfg;
}

// ---------------------------------------------------------------------------
// Базовые сценарии
// ---------------------------------------------------------------------------

TEST_CASE("CC: Челленджер регистрируется корректно", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    auto result = engine.register_challenger(
        StrategyId("champion_1"),
        StrategyId("challenger_1"),
        StrategyVersion(1));

    REQUIRE(result.has_value());

    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    REQUIRE(report->challengers.size() == 1);
    CHECK(report->challengers[0].challenger_id.get() == "challenger_1");
    CHECK(report->challengers[0].champion_id.get()   == "champion_1");
    CHECK(report->challengers[0].status == ChallengerStatus::Registered);
}

TEST_CASE("CC: Повторная регистрация отклоняется", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    auto result = engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(2));

    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Запись результатов и net P&L
// ---------------------------------------------------------------------------

TEST_CASE("CC: Результаты champion записываются (net P&L)", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    // 5 сделок по 10bps gross, без издержек → net = 50bps
    for (int i = 0; i < 5; ++i) {
        auto res = engine.record_champion_outcome(
            StrategyId("champion_1"), simple_pnl(10.0), "Trending");
        REQUIRE(res.has_value());
    }

    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    REQUIRE(report->challengers.size() == 1);
    CHECK(report->challengers[0].champion_metrics.decision_count == 5);
    CHECK_THAT(report->challengers[0].champion_metrics.net_pnl_bps,
        WithinAbs(50.0, 0.01));
    CHECK(report->challengers[0].status == ChallengerStatus::Evaluating);
}

TEST_CASE("CC: Комиссии и проскальзывание вычитаются из net P&L", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // gross=20, fee=3, slippage=2 → net = 15bps за сделку
    PnLBreakdown bd{.gross_pnl_bps = 20.0, .fee_bps = 3.0, .slippage_bps = 2.0};
    for (int i = 0; i < 3; ++i) {
        (void)engine.record_challenger_outcome(
            StrategyId("chall"), bd, "Trending", 0.8);
    }

    auto report = engine.evaluate(StrategyId("champ"));
    REQUIRE(report.has_value());

    const auto& cm = report->challengers[0].challenger_metrics;
    CHECK_THAT(cm.net_pnl_bps,        WithinAbs(45.0, 0.01));  // 3 × 15
    CHECK_THAT(cm.gross_pnl_bps,      WithinAbs(60.0, 0.01));  // 3 × 20
    CHECK_THAT(cm.total_fee_bps,      WithinAbs(9.0,  0.01));  // 3 × 3
    CHECK_THAT(cm.total_slippage_bps, WithinAbs(6.0,  0.01));  // 3 × 2
}

TEST_CASE("CC: Результаты challenger с conviction записываются", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    for (int i = 0; i < 5; ++i) {
        auto res = engine.record_challenger_outcome(
            StrategyId("challenger_1"), simple_pnl(15.0), "Trending", 0.8);
        REQUIRE(res.has_value());
    }

    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    const auto& cm = report->challengers[0].challenger_metrics;
    CHECK(cm.decision_count == 5);
    CHECK_THAT(cm.net_pnl_bps,    WithinAbs(75.0, 0.01));
    CHECK_THAT(cm.avg_conviction, WithinAbs(0.8,  0.01));
}

// ---------------------------------------------------------------------------
// Drawdown tracking
// ---------------------------------------------------------------------------

TEST_CASE("CC: Drawdown вычисляется корректно", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // 3 прибыльных → peak = 30bps
    for (int i = 0; i < 3; ++i)
        (void)engine.record_challenger_outcome(
            StrategyId("chall"), simple_pnl(10.0), "Trending", 0.8);

    // 2 убыточных: net накоплен = 30 - 15 - 15 = 0
    // После первого убытка dd = 15 - 30 = -15
    // После второго убытка dd = 0 - 30 = -30 → max_drawdown = -30
    for (int i = 0; i < 2; ++i)
        (void)engine.record_challenger_outcome(
            StrategyId("chall"), simple_pnl(-15.0), "Trending", 0.5);

    auto report = engine.evaluate(StrategyId("champ"));
    REQUIRE(report.has_value());
    const auto& cm = report->challengers[0].challenger_metrics;
    CHECK_THAT(cm.max_drawdown_bps, WithinAbs(-30.0, 0.01));
    CHECK_THAT(cm.net_pnl_bps,      WithinAbs(0.0,   0.01));
}

// ---------------------------------------------------------------------------
// Hit rate
// ---------------------------------------------------------------------------

TEST_CASE("CC: Hit rate вычисляется из прибыльных/убыточных сделок", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

(void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // 4 прибыльных, 1 убыточная → hit_rate = 4/5 = 0.8
    for (int i = 0; i < 4; ++i)
        (void)engine.record_challenger_outcome(
            StrategyId("chall"), simple_pnl(10.0), "Trending", 0.8);
    (void)engine.record_challenger_outcome(
        StrategyId("chall"), simple_pnl(-5.0), "Trending", 0.3);

    auto report = engine.evaluate(StrategyId("champ"));
    REQUIRE(report.has_value());
    CHECK_THAT(report->challengers[0].challenger_metrics.hit_rate(),
        WithinAbs(0.8, 0.01));
}

// ---------------------------------------------------------------------------
// Pre-promotion audit
// ---------------------------------------------------------------------------

TEST_CASE("CC: Промоушен при превышении всех порогов аудита", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Champion: 3 × 10bps gross = 30bps net
    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");

    // Challenger: 3 × 15bps gross = 45bps net (+50% delta, hit_rate=1.0, dd=0)
    for (int i = 0; i < 3; ++i)
        (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(15.0), "Trending", 0.9);

    // Аудит: delta=0.5 ≥ 0.2 ✓, hit_rate=1.0 ≥ 0.45 ✓, dd=0 ≥ -500 ✓
    CHECK(engine.should_promote(StrategyId("chall")));
    CHECK_FALSE(engine.should_reject(StrategyId("chall")));

    auto result = engine.promote(StrategyId("chall"));
    REQUIRE(result.has_value());

    auto report = engine.evaluate(StrategyId("champ"));
    CHECK(report->challengers[0].status == ChallengerStatus::Promoted);
}

TEST_CASE("CC: Промоушен блокируется при низком hit rate", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());  // min_hit_rate = 0.45

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");

    // 1 крупный выигрыш + 4 проигрыша: net delta > порога, но hit_rate = 1/5 = 0.2 < 0.45
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(200.0), "Trending", 0.9);
    for (int i = 0; i < 4; ++i)
        (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(-5.0), "Trending", 0.3);

    CHECK_FALSE(engine.should_promote(StrategyId("chall")));  // Fails hit rate audit

    auto audit = engine.audit_challenger(StrategyId("chall"));
    CHECK_FALSE(audit.hit_rate_adequate);
    CHECK_FALSE(audit.failure_reason.empty());
}

TEST_CASE("CC: Промоушен блокируется при глубокой просадке", "[champion_challenger]") {
    // Жёсткий лимит просадки для этого теста
    ChampionChallengerConfig strict_cfg = default_config();
    strict_cfg.max_drawdown_bps = -20.0;  // Не хуже -20bps
    ChampionChallengerEngine engine(strict_cfg);

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");

    // Challenger: хорошая финальная производительность, но глубокая просадка
    // net_pnl: +30, -50, +100 → финальный +80bps, но в середине dd = -50 - 30 = -80 < -20
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(30.0), "Trending",  0.9);
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(-50.0), "Trending", 0.2);
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(100.0), "Trending", 0.9);

    CHECK_FALSE(engine.should_promote(StrategyId("chall")));  // Fails drawdown audit

    auto audit = engine.audit_challenger(StrategyId("chall"));
    CHECK_FALSE(audit.max_drawdown_acceptable);
}

// ---------------------------------------------------------------------------
// Отклонение
// ---------------------------------------------------------------------------

TEST_CASE("CC: Отклонение при недостаточной производительности", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Champion: 3 × 20bps = 60bps
    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(20.0), "Trending");

    // Challenger: 3 × 10bps = 30bps → delta = (30-60)/60 = -0.5 ≤ -0.1
    for (int i = 0; i < 3; ++i)
        (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(10.0), "Trending", 0.5);

    CHECK_FALSE(engine.should_promote(StrategyId("chall")));
    CHECK(engine.should_reject(StrategyId("chall")));

    auto result = engine.reject(StrategyId("chall"));
    REQUIRE(result.has_value());

    auto report = engine.evaluate(StrategyId("champ"));
    CHECK(report->challengers[0].status == ChallengerStatus::Rejected);
}

// ---------------------------------------------------------------------------
// Observer callbacks
// ---------------------------------------------------------------------------

TEST_CASE("CC: Observer получает on_promotion callback", "[champion_challenger]") {
    struct TestObserver : IChallengerObserver {
        int promotions{0}, rejections{0};
        void on_promotion(const ChallengerEntry&) override { ++promotions; }
        void on_rejection(const ChallengerEntry&) override { ++rejections; }
    };

    ChampionChallengerEngine engine(default_config());
    auto obs = std::make_shared<TestObserver>();
    engine.add_observer(obs);

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");
    for (int i = 0; i < 3; ++i)
        (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(20.0), "Trending", 0.9);

    (void)engine.promote(StrategyId("chall"));

    CHECK(obs->promotions == 1);
    CHECK(obs->rejections == 0);
}

TEST_CASE("CC: Observer получает on_rejection callback", "[champion_challenger]") {
    struct TestObserver : IChallengerObserver {
        int rejections{0};
        void on_promotion(const ChallengerEntry&) override {}
        void on_rejection(const ChallengerEntry&) override { ++rejections; }
    };

    ChampionChallengerEngine engine(default_config());
    auto obs = std::make_shared<TestObserver>();
    engine.add_observer(obs);

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    for (int i = 0; i < 3; ++i)
        (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(20.0), "Trending");
    for (int i = 0; i < 3; ++i)
        (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(8.0), "Trending", 0.5);

    (void)engine.reject(StrategyId("chall"));
    CHECK(obs->rejections == 1);
}

// ---------------------------------------------------------------------------
// Граничные случаи
// ---------------------------------------------------------------------------

TEST_CASE("CC: Недостаточно данных — не промоутим и не реджектим", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());  // min_evaluation_trades = 3

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Только 2 сделки challenger (< 3)
    (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");
    (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(100.0), "Trending", 0.9);
    (void)engine.record_challenger_outcome(StrategyId("chall"), simple_pnl(100.0), "Trending", 0.9);

    CHECK_FALSE(engine.should_promote(StrategyId("chall")));
    CHECK_FALSE(engine.should_reject(StrategyId("chall")));
}

TEST_CASE("CC: Незарегистрированная стратегия-challenger возвращает ошибку", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    // record_champion_outcome молча игнорирует неизвестного champion-а (ok для pipeline)
    auto champion_res = engine.record_champion_outcome(
        StrategyId("no_such_champ"), simple_pnl(10.0), "Trending");
    REQUIRE(champion_res.has_value());  // Silently ignored

    // record_challenger_outcome возвращает ошибку для незарегистрированного challenger-а
    auto challenger_res = engine.record_challenger_outcome(
        StrategyId("no_such_chall"), simple_pnl(10.0), "Trending", 0.8);
    REQUIRE_FALSE(challenger_res.has_value());
}

TEST_CASE("CC: Отчёт для несуществующего champion пуст", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    auto report = engine.evaluate(StrategyId("nonexistent"));
    REQUIRE(report.has_value());
    CHECK(report->challengers.empty());
}

TEST_CASE("CC: Два challenger для одного champion", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall_a"), StrategyVersion(1));
    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall_b"), StrategyVersion(2));

    (void)engine.record_champion_outcome(StrategyId("champ"), simple_pnl(10.0), "Trending");
    (void)engine.record_challenger_outcome(StrategyId("chall_a"), simple_pnl(15.0), "Trending", 0.8);
    (void)engine.record_challenger_outcome(StrategyId("chall_b"), simple_pnl(5.0),  "Ranging",  0.4);

    auto report = engine.evaluate(StrategyId("champ"));
    REQUIRE(report.has_value());
    CHECK(report->champion_id.get() == "champ");
    CHECK(report->challengers.size() == 2);
}
