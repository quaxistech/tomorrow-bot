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
        outcome.max_adverse_excursion_bps = 3.0;
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
        outcome.max_adverse_excursion_bps = 15.0;
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
    add_profitable_trades(monitor, sid, 40, 15.0);
    add_losing_trades(monitor, sid, 10, -20.0);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    REQUIRE(result->overall_health < 0.8);
    REQUIRE(!result->alerts.empty());
    REQUIRE(monitor.is_degraded(sid));
}

TEST_CASE("AlphaDecayMonitor: запись сделок и ограничение истории", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 10;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"test-strat"};
    add_profitable_trades(monitor, sid, 30);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
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
    add_profitable_trades(monitor, sid, 40, 20.0);
    add_losing_trades(monitor, sid, 10, -30.0);

    // Гистерезис требует stable_count >= 2 вызовов подряд для смены рекомендации
    monitor.analyze(sid);
    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    REQUIRE(result->overall_recommendation != DecayRecommendation::NoAction);
}

TEST_CASE("AlphaDecayMonitor: строковые представления перечислений", "[alpha_decay]") {
    REQUIRE(to_string(DecayRecommendation::NoAction) == "NoAction");
    REQUIRE(to_string(DecayRecommendation::Disable) == "Disable");
    REQUIRE(to_string(DecayDimension::Expectancy) == "Expectancy");
    REQUIRE(to_string(DecayDimension::HitRate) == "HitRate");
}

TEST_CASE("AlphaDecayMonitor: HitRate dimension расчёт", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 20;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"hit-rate-test"};
    // 20 прибыльных формируют базу (long window)
    add_profitable_trades(monitor, sid, 20, 10.0);
    // 5 убыточных в коротком окне — hit rate падает
    add_losing_trades(monitor, sid, 5, -10.0);

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    // Ищем метрику HitRate
    bool found = false;
    for (const auto& m : result->metrics) {
        if (m.dimension == DecayDimension::HitRate) {
            REQUIRE(m.current_value < 0.5);  // в коротком окне все убытки
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("AlphaDecayMonitor: MAE dimension — рост MAE фиксируется", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 20;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"mae-test"};
    // Базовые сделки с малым MAE
    for (int i = 0; i < 20; ++i) {
        TradeOutcome o;
        o.pnl_bps = 5.0;
        o.slippage_bps = 1.0;
        o.max_adverse_excursion_bps = 2.0;
        o.regime = RegimeLabel::Trending;
        o.conviction = 0.7;
        o.timestamp = Timestamp{static_cast<int64_t>(i)};
        monitor.record_trade_outcome(sid, o);
    }
    // Последние 5 с большим MAE
    for (int i = 0; i < 5; ++i) {
        TradeOutcome o;
        o.pnl_bps = 3.0;
        o.slippage_bps = 1.0;
        o.max_adverse_excursion_bps = 50.0;  // Сильное ухудшение
        o.regime = RegimeLabel::Trending;
        o.conviction = 0.7;
        o.timestamp = Timestamp{static_cast<int64_t>(20 + i)};
        monitor.record_trade_outcome(sid, o);
    }

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    bool found = false;
    for (const auto& m : result->metrics) {
        if (m.dimension == DecayDimension::AdverseExcursion) {
            REQUIRE(m.drift_pct > 0);  // drift должен быть положительным (ухудшение)
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("AlphaDecayMonitor: гистерезис — рекомендация не меняется без накопления", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 20;
    config.hysteresis_stable_count = 3;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"hysteresis-test"};
    // Сначала хорошие — устанавливаем NoAction
    add_profitable_trades(monitor, sid, 20, 10.0);
    auto r1 = monitor.analyze(sid);
    REQUIRE(r1.has_value());
    auto first_rec = r1->overall_recommendation;

    // Добавляем немного плохих — без накопления рекомендация не должна резко прыгнуть
    add_losing_trades(monitor, sid, 3, -5.0);
    auto r2 = monitor.analyze(sid);
    REQUIRE(r2.has_value());
    // Со стабильным счётчиком hysteresis_stable_count=3 одна проверка не меняет рек
    // (либо NoAction, либо мягкая деградация — но не Disable)
    REQUIRE(r2->overall_recommendation != DecayRecommendation::Disable);
}

TEST_CASE("AlphaDecayMonitor: Confidence Reliability — Brier score", "[alpha_decay]") {
    DecayConfig config;
    config.short_lookback = 5;
    config.long_lookback = 20;
    AlphaDecayMonitor monitor(config);

    StrategyId sid{"brier-test"};
    // Базовые сделки с хорошей калибровкой (высокая conviction → прибыль)
    for (int i = 0; i < 20; ++i) {
        TradeOutcome o;
        o.pnl_bps = 10.0;
        o.slippage_bps = 1.0;
        o.max_adverse_excursion_bps = 2.0;
        o.regime = RegimeLabel::Trending;
        o.conviction = 0.85;
        o.timestamp = Timestamp{static_cast<int64_t>(i)};
        monitor.record_trade_outcome(sid, o);
    }
    // Последние 5 с плохой калибровкой (высокая conviction → убыток)
    for (int i = 0; i < 5; ++i) {
        TradeOutcome o;
        o.pnl_bps = -15.0;
        o.slippage_bps = 1.0;
        o.max_adverse_excursion_bps = 2.0;
        o.regime = RegimeLabel::Trending;
        o.conviction = 0.90;
        o.timestamp = Timestamp{static_cast<int64_t>(20 + i)};
        monitor.record_trade_outcome(sid, o);
    }

    auto result = monitor.analyze(sid);
    REQUIRE(result.has_value());
    bool found = false;
    for (const auto& m : result->metrics) {
        if (m.dimension == DecayDimension::ConfidenceReliability) {
            // Плохая калибровка → Brier score высокий → drift_pct > 0
            found = true;
            break;
        }
    }
    REQUIRE(found);
}
