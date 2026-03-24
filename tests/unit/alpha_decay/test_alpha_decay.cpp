/**
 * @file test_alpha_decay.cpp
 * @brief Тесты монитора угасания альфы
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "alpha_decay/alpha_decay_monitor.hpp"

using namespace tb;
using namespace tb::alpha_decay;

// Генерирует серию прибыльных сделок
inline void add_profitable_trades(AlphaDecayMonitor& monitor,
                                   const StrategyId& sid, int count, double pnl = 10.0) {
    for (int i = 0; i < count; ++i) {
        TradeOutcome outcome;
        outcome.pnl_bps = pnl;
        outcome.slippage_bps = 1.0;
        outcome.regime = RegimeLabel::Trending;
        outcome.conviction = 0.8;
        outcome.timestamp = Timestamp{static_cast<int64_t>(i * 1000)};
        monitor.record_trade_outcome(sid, outcome);
    }
}

// Генерирует серию убыточных сделок
inline void add_losing_trades(AlphaDecayMonitor& monitor,
                               const StrategyId& sid, int count, double pnl = -10.0) {
    for (int i = 0; i < count; ++i) {
        TradeOutcome outcome;
        outcome.pnl_bps = pnl;
        outcome.slippage_bps = 5.0;
        outcome.regime = RegimeLabel::Volatile;
        outcome.conviction = 0.3;
        outcome.timestamp = Timestamp{static_cast<int64_t>(i * 1000)};
        monitor.record_trade_outcome(sid, outcome);
    }
}

TEST_CASE("AlphaDecayMonitor: анализ несуществующей стратегии", "[alpha_decay]") {
    AlphaDecayMonitor monitor;
    auto result = monitor.analyze(StrategyId{"nonexistent"});
    REQUIRE(!result.has_value());
}

TEST_CASE("AlphaDecayMonitor: здоровая стратегия", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 10;
    config.long_lookback = 50;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"momentum-v1"};
    // Все сделки прибыльные — стабильная стратегия
    add_profitable_trades(monitor, sid, 50, 10.0);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    REQUIRE(result->overall_health > 0.8);
    REQUIRE(result->overall_recommendation == DecayRecommendation::NoAction);
    REQUIRE(result->strategy_id.get() == "momentum-v1");
}

TEST_CASE("AlphaDecayMonitor: деградация стратегии", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 10;
    config.long_lookback = 50;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"mean-revert-v2"};
    // Сначала хорошие сделки (длинное окно)
    add_profitable_trades(monitor, sid, 40, 15.0);
    // Потом плохие (попадут в короткое окно)
    add_losing_trades(monitor, sid, 10, -20.0);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    REQUIRE(result->overall_health < 0.8);
    // Должны быть алерты
    REQUIRE(!result->alerts.empty());

    // Проверяем is_degraded
    REQUIRE(monitor.is_degraded(sid));
}

TEST_CASE("AlphaDecayMonitor: запись сделок и ограничение истории", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 10;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"test-strat"};
    // Добавляем больше 2x long_lookback сделок
    add_profitable_trades(monitor, sid, 30);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    // Не должно быть ошибок несмотря на обрезку
}

TEST_CASE("AlphaDecayMonitor: get_all_reports для нескольких стратегий", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 20;
    AlphaDecayMonitor monitor(config);

    StrategyId s1{"strat-1"};
    StrategyId s2{"strat-2"};
    add_profitable_trades(monitor, s1, 25);
    add_profitable_trades(monitor, s2, 25);

    auto reports = monitor.get_all_reports();
    REQUIRE(reports.size() == 2);
}

TEST_CASE("AlphaDecayMonitor: рекомендации по уровню здоровья", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 10;
    config.long_lookback = 50;
    config.health_warning_threshold = 0.5;
    config.health_critical_threshold = 0.3;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"degrading"};
    // Сначала хорошие — формируют базовый уровень
    add_profitable_trades(monitor, sid, 40, 20.0);
    // Потом резко плохие — формируют деградацию
    add_losing_trades(monitor, sid, 10, -30.0);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    // Должна быть рекомендация отличная от NoAction
    REQUIRE(result->overall_recommendation != DecayRecommendation::NoAction);
}

TEST_CASE("AlphaDecayMonitor: строковые представления перечислений", "[alpha_decay]") {
    REQUIRE(to_string(DecayRecommendation::NoAction) == "NoAction");
    REQUIRE(to_string(DecayRecommendation::Disable) == "Disable");
    REQUIRE(to_string(DecayDimension::Expectancy) == "Expectancy");
    REQUIRE(to_string(DecayDimension::HitRate) == "HitRate");
}
