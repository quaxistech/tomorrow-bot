/**
 * @file test_champion_challenger.cpp
 * @brief Тесты Champion-Challenger A/B тестирования
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "champion_challenger/champion_challenger_engine.hpp"

using namespace tb;
using namespace tb::champion_challenger;
using namespace Catch::Matchers;

/// Конфигурация по умолчанию для тестов
static ChampionChallengerConfig default_config() {
    return {
        .min_evaluation_trades = 3,     // Снижен для тестов
        .promotion_threshold = 0.2,     // +20%
        .rejection_threshold = -0.1     // -10%
    };
}

TEST_CASE("CC: Челленджер регистрируется корректно", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    auto result = engine.register_challenger(
        StrategyId("champion_1"),
        StrategyId("challenger_1"),
        StrategyVersion(1));

    REQUIRE(result.has_value());

    // Проверяем через evaluate
    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    REQUIRE(report->challengers.size() == 1);
    CHECK(report->challengers[0].challenger_id.get() == "challenger_1");
    CHECK(report->challengers[0].champion_id.get() == "champion_1");
    CHECK(report->challengers[0].status == ChallengerStatus::Registered);
}

TEST_CASE("CC: Повторная регистрация отклоняется", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    auto result = engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(2));

    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CC: Результаты champion записываются", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    // Записываем результаты champion
    for (int i = 0; i < 5; ++i) {
        auto res = engine.record_champion_outcome(
            StrategyId("champion_1"), 10.0, "Trending");
        REQUIRE(res.has_value());
    }

    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    REQUIRE(report->challengers.size() == 1);
    CHECK(report->challengers[0].champion_metrics.decision_count == 5);
    CHECK_THAT(report->challengers[0].champion_metrics.hypothetical_pnl_bps,
        WithinAbs(50.0, 0.01));
    CHECK(report->challengers[0].status == ChallengerStatus::Evaluating);
}

TEST_CASE("CC: Результаты challenger записываются", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    engine.register_challenger(
        StrategyId("champion_1"), StrategyId("challenger_1"), StrategyVersion(1));

    for (int i = 0; i < 5; ++i) {
        auto res = engine.record_challenger_outcome(
            StrategyId("challenger_1"), 15.0, "Trending", 0.8);
        REQUIRE(res.has_value());
    }

    auto report = engine.evaluate(StrategyId("champion_1"));
    REQUIRE(report.has_value());
    REQUIRE(report->challengers.size() == 1);
    CHECK(report->challengers[0].challenger_metrics.decision_count == 5);
    CHECK_THAT(report->challengers[0].challenger_metrics.hypothetical_pnl_bps,
        WithinAbs(75.0, 0.01));
    CHECK_THAT(report->challengers[0].challenger_metrics.avg_conviction,
        WithinAbs(0.8, 0.01));
}

TEST_CASE("CC: Промоушен при превышении порога", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Champion: 3 сделки по 10 bps = 30 bps
    for (int i = 0; i < 3; ++i) {
        (void)engine.record_champion_outcome(StrategyId("champ"), 10.0, "Trending");
    }

    // Challenger: 3 сделки по 15 bps = 45 bps → +50% лучше
    for (int i = 0; i < 3; ++i) {
        (void)engine.record_challenger_outcome(StrategyId("chall"), 15.0, "Trending", 0.9);
    }

    // (45 - 30) / 30 = 0.5 > 0.2 (порог) → промоушен
    CHECK(engine.should_promote(StrategyId("chall")));
    CHECK_FALSE(engine.should_reject(StrategyId("chall")));

    auto result = engine.promote(StrategyId("chall"));
    REQUIRE(result.has_value());

    auto report = engine.evaluate(StrategyId("champ"));
    CHECK(report->challengers[0].status == ChallengerStatus::Promoted);
}

TEST_CASE("CC: Отклонение при недостаточной производительности", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Champion: 3 сделки по 20 bps = 60 bps
    for (int i = 0; i < 3; ++i) {
        (void)engine.record_champion_outcome(StrategyId("champ"), 20.0, "Trending");
    }

    // Challenger: 3 сделки по 10 bps = 30 bps → -50% хуже
    for (int i = 0; i < 3; ++i) {
        (void)engine.record_challenger_outcome(StrategyId("chall"), 10.0, "Trending", 0.5);
    }

    // (30 - 60) / 60 = -0.5 < -0.1 (порог) → отклонение
    CHECK_FALSE(engine.should_promote(StrategyId("chall")));
    CHECK(engine.should_reject(StrategyId("chall")));

    auto result = engine.reject(StrategyId("chall"));
    REQUIRE(result.has_value());

    auto report = engine.evaluate(StrategyId("champ"));
    CHECK(report->challengers[0].status == ChallengerStatus::Rejected);
}

TEST_CASE("CC: Отчёт генерируется корректно", "[champion_challenger]") {
    ChampionChallengerEngine engine(default_config());

    // Два челленджера для одного champion
    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall_a"), StrategyVersion(1));
    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall_b"), StrategyVersion(2));

    (void)engine.record_champion_outcome(StrategyId("champ"), 10.0, "Trending");
    (void)engine.record_challenger_outcome(StrategyId("chall_a"), 15.0, "Trending", 0.8);
    (void)engine.record_challenger_outcome(StrategyId("chall_b"), 5.0, "Ranging", 0.4);

    auto report = engine.evaluate(StrategyId("champ"));
    REQUIRE(report.has_value());
    CHECK(report->champion_id.get() == "champ");
    CHECK(report->challengers.size() == 2);

    // Отчёт для несуществующего champion — пустой
    auto empty_report = engine.evaluate(StrategyId("nonexistent"));
    REQUIRE(empty_report.has_value());
    CHECK(empty_report->challengers.empty());
}

TEST_CASE("CC: Недостаточно данных — не промоутим и не реджектим", "[champion_challenger]") {
    // min_evaluation_trades = 3
    ChampionChallengerEngine engine(default_config());

    (void)engine.register_challenger(
        StrategyId("champ"), StrategyId("chall"), StrategyVersion(1));

    // Только 2 сделки challenger (< 3)
    (void)engine.record_champion_outcome(StrategyId("champ"), 10.0, "Trending");
    (void)engine.record_champion_outcome(StrategyId("champ"), 10.0, "Trending");
    (void)engine.record_challenger_outcome(StrategyId("chall"), 100.0, "Trending", 0.9);
    (void)engine.record_challenger_outcome(StrategyId("chall"), 100.0, "Trending", 0.9);

    // Недостаточно данных — оба false
    CHECK_FALSE(engine.should_promote(StrategyId("chall")));
    CHECK_FALSE(engine.should_reject(StrategyId("chall")));
}
