/**
 * @file test_adversarial_defense.cpp
 * @brief Тесты модуля защиты от враждебных рыночных условий
 */

#include <catch2/catch_all.hpp>

#include "adversarial_defense/adversarial_defense.hpp"
#include "adversarial_defense/adversarial_types.hpp"

using namespace tb;
using namespace tb::adversarial;

/// Создание безопасных рыночных условий
static MarketCondition make_safe_condition() {
    return MarketCondition{
        .symbol = Symbol("BTCUSDT"),
        .spread_bps = 5.0,
        .book_imbalance = 0.2,
        .bid_depth = 200.0,
        .ask_depth = 200.0,
        .book_instability = 0.1,
        .buy_sell_ratio = 0.5,
        .book_valid = true,
        .timestamp = Timestamp(1'000'000'000)
    };
}

TEST_CASE("AdversarialMarketDefense — безопасные условия", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    auto condition = make_safe_condition();

    auto assessment = defense.assess(condition);

    CHECK(assessment.is_safe);
    CHECK(assessment.overall_action == DefenseAction::NoAction);
    CHECK(assessment.threats.empty());
    CHECK(assessment.confidence_multiplier == 1.0);
    CHECK(assessment.threshold_multiplier == 1.0);
    CHECK_FALSE(assessment.cooldown_active);
}

TEST_CASE("AdversarialMarketDefense — взрыв спреда", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    auto condition = make_safe_condition();
    condition.spread_bps = 200.0; // Превышает порог 100 bps

    auto assessment = defense.assess(condition);

    CHECK_FALSE(assessment.is_safe);
    CHECK(assessment.overall_action == DefenseAction::VetoTrade);
    REQUIRE_FALSE(assessment.threats.empty());

    // Ищем угрозу взрыва спреда
    bool found = false;
    for (const auto& t : assessment.threats) {
        if (t.type == ThreatType::SpreadExplosion) {
            found = true;
            CHECK(t.severity > 0.0);
            CHECK(t.recommended_action == DefenseAction::VetoTrade);
        }
    }
    CHECK(found);
}

TEST_CASE("AdversarialMarketDefense — вакуум ликвидности", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    auto condition = make_safe_condition();
    condition.bid_depth = 5.0;  // Очень низкая глубина
    condition.ask_depth = 3.0;

    auto assessment = defense.assess(condition);

    CHECK_FALSE(assessment.is_safe);
    CHECK(assessment.overall_action == DefenseAction::VetoTrade);

    bool found = false;
    for (const auto& t : assessment.threats) {
        if (t.type == ThreatType::LiquidityVacuum) {
            found = true;
            CHECK(t.severity > 0.7);
        }
    }
    CHECK(found);
}

TEST_CASE("AdversarialMarketDefense — нестабильный стакан", "[adversarial_defense]") {
    AdversarialMarketDefense defense;

    SECTION("Высокая нестабильность") {
        auto condition = make_safe_condition();
        condition.book_instability = 0.9; // Превышает порог 0.7

        auto assessment = defense.assess(condition);

        bool found = false;
        for (const auto& t : assessment.threats) {
            if (t.type == ThreatType::UnstableOrderBook) {
                found = true;
                CHECK(t.recommended_action == DefenseAction::ReduceConfidence);
            }
        }
        CHECK(found);
        CHECK(assessment.confidence_multiplier < 1.0);
    }

    SECTION("Невалидный стакан") {
        auto condition = make_safe_condition();
        condition.book_valid = false;

        auto assessment = defense.assess(condition);

        CHECK_FALSE(assessment.is_safe);

        bool found = false;
        for (const auto& t : assessment.threats) {
            if (t.type == ThreatType::UnstableOrderBook) {
                found = true;
                CHECK(t.severity == 1.0);
                CHECK(t.recommended_action == DefenseAction::VetoTrade);
            }
        }
        CHECK(found);
    }
}

TEST_CASE("AdversarialMarketDefense — токсичный поток", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    auto condition = make_safe_condition();
    condition.buy_sell_ratio = 0.95; // Превышает порог 0.85

    auto assessment = defense.assess(condition);

    bool found = false;
    for (const auto& t : assessment.threats) {
        if (t.type == ThreatType::ToxicFlow) {
            found = true;
            CHECK(t.recommended_action == DefenseAction::RaiseThreshold);
        }
    }
    CHECK(found);
    CHECK(assessment.threshold_multiplier > 1.0);
}

TEST_CASE("AdversarialMarketDefense — cooldown после шока", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    Symbol sym("BTCUSDT");

    // Регистрируем шок в момент 1000 мс (в наносекундах)
    Timestamp shock_time(1'000'000'000); // 1000 мс в нано
    defense.register_shock(sym, ThreatType::SpreadExplosion, shock_time);

    // Проверяем через 1 секунду — cooldown должен быть активен (длительность 30с)
    Timestamp check_time(2'000'000'000); // 2000 мс в нано
    CHECK(defense.is_cooldown_active(sym, check_time));

    // Оцениваем рыночную обстановку во время cooldown
    auto condition = make_safe_condition();
    condition.timestamp = check_time;

    auto assessment = defense.assess(condition);

    CHECK_FALSE(assessment.is_safe);
    CHECK(assessment.cooldown_active);
    CHECK(assessment.cooldown_remaining_ms > 0);
}

TEST_CASE("AdversarialMarketDefense — сброс cooldown", "[adversarial_defense]") {
    AdversarialMarketDefense defense;
    Symbol sym("BTCUSDT");

    Timestamp shock_time(1'000'000'000);
    defense.register_shock(sym, ThreatType::SpreadExplosion, shock_time);

    // Cooldown активен
    CHECK(defense.is_cooldown_active(sym, Timestamp(2'000'000'000)));

    // Сбрасываем
    defense.reset_cooldown(sym);

    // Cooldown больше не активен
    CHECK_FALSE(defense.is_cooldown_active(sym, Timestamp(2'000'000'000)));
}

TEST_CASE("AdversarialMarketDefense — to_string функции", "[adversarial_defense]") {
    CHECK(to_string(ThreatType::SpreadExplosion) == "SpreadExplosion");
    CHECK(to_string(ThreatType::LiquidityVacuum) == "LiquidityVacuum");
    CHECK(to_string(DefenseAction::VetoTrade) == "VetoTrade");
    CHECK(to_string(DefenseAction::NoAction) == "NoAction");
}
