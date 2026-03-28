#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "world_model/world_model_engine.hpp"
#include "world_model/world_model_config.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

using namespace tb;
using namespace tb::world_model;
using namespace tb::features;

// ============================================================
// Вспомогательные классы для тестов
// ============================================================

namespace {

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

class TestClock : public clock::IClock {
public:
    [[nodiscard]] Timestamp now() const override { return Timestamp(1000000); }
};

FeatureSnapshot make_snapshot(const std::string& sym = "BTCUSDT") {
    FeatureSnapshot snap;
    snap.symbol = Symbol(sym);
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    snap.execution_context.is_feed_fresh = true;
    return snap;
}

FeatureSnapshot make_trend_snapshot() {
    auto snap = make_snapshot();
    snap.technical.adx = 30.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 55.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.technical.ema_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;
    return snap;
}

std::shared_ptr<TestLogger> make_logger() { return std::make_shared<TestLogger>(); }
std::shared_ptr<TestClock> make_clock() { return std::make_shared<TestClock>(); }

RuleBasedWorldModelEngine make_engine() {
    return RuleBasedWorldModelEngine(make_logger(), make_clock());
}

RuleBasedWorldModelEngine make_engine_with_config(WorldModelConfig config) {
    return RuleBasedWorldModelEngine(std::move(config), make_logger(), make_clock());
}

} // anonymous namespace

// ============================================================
// Классификация: все 9 состояний
// ============================================================

TEST_CASE("WorldModel: StableTrendContinuation — высокий ADX, умеренный RSI", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::StableTrendContinuation);
    REQUIRE(result.label == WorldStateLabel::Stable);
    REQUIRE(result.fragility.valid);
    REQUIRE(result.fragility.value < 0.5);
    REQUIRE(result.confidence > 0.0);
    REQUIRE(!result.model_version.empty());
}

TEST_CASE("WorldModel: ChopNoise — низкий ADX, средний RSI", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::ChopNoise);
    REQUIRE(result.label == WorldStateLabel::Transitioning);
}

TEST_CASE("WorldModel: ExhaustionSpike — экстремальный RSI", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.rsi_14 = 85.0;
    snap.technical.rsi_valid = true;
    snap.technical.momentum_5 = 0.05;
    snap.technical.momentum_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::ExhaustionSpike);
    REQUIRE(result.label == WorldStateLabel::Disrupted);
    REQUIRE(result.fragility.value > 0.7);
}

TEST_CASE("WorldModel: LiquidityVacuum — очень широкий спред", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 60.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::LiquidityVacuum);
    REQUIRE(result.label == WorldStateLabel::Disrupted);
    REQUIRE(result.fragility.value > 0.8);
}

TEST_CASE("WorldModel: LiquidityVacuum — перекос ликвидности", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 25.0;
    snap.microstructure.liquidity_valid = true;
    snap.microstructure.liquidity_ratio = 0.2;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::LiquidityVacuum);
}

TEST_CASE("WorldModel: ToxicMicrostructure — book instability + aggressive flow", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.sma_valid = true;
    snap.microstructure.instability_valid = true;
    snap.microstructure.book_instability = 0.85;
    snap.microstructure.trade_flow_valid = true;
    snap.microstructure.aggressive_flow = 0.9;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 20.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::ToxicMicrostructure);
    REQUIRE(result.label == WorldStateLabel::Disrupted);
}

TEST_CASE("WorldModel: FragileBreakout — BB edge + volatility + book imbalance", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.bb_valid = true;
    snap.technical.bb_percent_b = 0.98;
    snap.technical.volatility_valid = true;
    snap.technical.volatility_5 = 0.03;
    snap.technical.sma_valid = true;
    snap.microstructure.book_imbalance_valid = true;
    snap.microstructure.book_imbalance_5 = 0.5;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::FragileBreakout);
    REQUIRE(result.label == WorldStateLabel::Transitioning);
}

TEST_CASE("WorldModel: CompressionBeforeExpansion — узкий BB + низкий ATR", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.bb_valid = true;
    snap.technical.bb_bandwidth = 0.02;
    snap.technical.atr_valid = true;
    snap.technical.atr_14_normalized = 0.005;
    snap.technical.volatility_valid = true;
    snap.technical.volatility_5 = 0.005;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::CompressionBeforeExpansion);
    REQUIRE(result.label == WorldStateLabel::Transitioning);
}

TEST_CASE("WorldModel: PostShockStabilization — после ExhaustionSpike", "[world_model]") {
    auto engine = make_engine();

    // Шаг 1: создаём ExhaustionSpike
    auto snap1 = make_snapshot();
    snap1.technical.rsi_14 = 85.0;
    snap1.technical.rsi_valid = true;
    snap1.technical.momentum_5 = 0.05;
    snap1.technical.momentum_valid = true;
    snap1.technical.sma_valid = true;
    snap1.microstructure.spread_valid = true;
    snap1.microstructure.spread_bps = 5.0;
    auto r1 = engine.update(snap1);
    REQUIRE(r1.state == WorldState::ExhaustionSpike);

    // Шаг 2: volatility снижается → PostShockStabilization
    auto snap2 = make_snapshot();
    snap2.technical.volatility_valid = true;
    snap2.technical.volatility_5 = 0.01;
    snap2.technical.volatility_20 = 0.03;
    snap2.technical.sma_valid = true;
    snap2.microstructure.spread_valid = true;
    snap2.microstructure.spread_bps = 5.0;
    auto r2 = engine.update(snap2);

    REQUIRE(r2.state == WorldState::PostShockStabilization);
}

TEST_CASE("WorldModel: Unknown — нет данных", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();

    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::Unknown);
    REQUIRE(result.label == WorldStateLabel::Unknown);
}

// ============================================================
// to_label маппинг
// ============================================================

TEST_CASE("WorldModel: to_label корректность маппинга", "[world_model]") {
    REQUIRE(WorldModelSnapshot::to_label(WorldState::StableTrendContinuation) == WorldStateLabel::Stable);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::FragileBreakout) == WorldStateLabel::Transitioning);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ExhaustionSpike) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::LiquidityVacuum) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ToxicMicrostructure) == WorldStateLabel::Disrupted);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::Unknown) == WorldStateLabel::Unknown);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::ChopNoise) == WorldStateLabel::Transitioning);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::CompressionBeforeExpansion) == WorldStateLabel::Transitioning);
    REQUIRE(WorldModelSnapshot::to_label(WorldState::PostShockStabilization) == WorldStateLabel::Transitioning);
}

// ============================================================
// current_state и кеширование
// ============================================================

TEST_CASE("WorldModel: current_state возвращает последний снимок", "[world_model]") {
    auto engine = make_engine();

    REQUIRE_FALSE(engine.current_state(Symbol("BTCUSDT")).has_value());

    auto snap = make_trend_snapshot();
    engine.update(snap);

    auto state = engine.current_state(Symbol("BTCUSDT"));
    REQUIRE(state.has_value());
    REQUIRE(state->state == WorldState::StableTrendContinuation);
}

// ============================================================
// Suitability
// ============================================================

TEST_CASE("WorldModel: strategy_suitability заполняется с multi-dimension", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE_FALSE(result.strategy_suitability.empty());

    bool found_momentum = false;
    for (const auto& s : result.strategy_suitability) {
        if (s.strategy_id.get() == "momentum") {
            found_momentum = true;
            REQUIRE(s.suitability > 0.5);
            REQUIRE(s.signal_suitability > 0.5);
            REQUIRE(s.execution_suitability > 0.0);
            REQUIRE(s.risk_suitability > 0.0);
        }
    }
    REQUIRE(found_momentum);
}

TEST_CASE("WorldModel: suitability veto в опасных состояниях", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_snapshot();
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 60.0;
    auto result = engine.update(snap);

    REQUIRE(result.state == WorldState::LiquidityVacuum);

    for (const auto& s : result.strategy_suitability) {
        if (s.strategy_id.get() == "microstructure_scalp") {
            REQUIRE(s.vetoed);
            REQUIRE(s.suitability == 0.0);
        }
    }
}

// ============================================================
// Конфигурируемые пороги
// ============================================================

TEST_CASE("WorldModel: конфигурируемые пороги меняют классификацию", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.stable_trend.adx_min = 10.0; // Сильно снижаем порог

    auto engine = make_engine_with_config(config);
    auto snap = make_snapshot();
    snap.technical.adx = 12.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 55.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;

    auto result = engine.update(snap);

    // С default config (adx_min=25) это был бы Unknown или ChopNoise
    // С adx_min=10 это StableTrendContinuation
    REQUIRE(result.state == WorldState::StableTrendContinuation);
}

// ============================================================
// Explainability
// ============================================================

TEST_CASE("WorldModel: explanation содержит проверенные условия", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE(!result.explanation.checked_conditions.empty());
    REQUIRE(!result.explanation.triggered_conditions.empty());
    REQUIRE(result.explanation.valid_indicator_count > 0);
    REQUIRE(result.explanation.total_indicator_count > 0);
    REQUIRE(result.explanation.data_quality_score > 0.0);
    REQUIRE(!result.explanation.summary.empty());
}

TEST_CASE("WorldModel: top_drivers заполняются", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();
    snap.technical.volatility_valid = true;
    snap.technical.volatility_5 = 0.015;
    snap.technical.volatility_20 = 0.02;
    auto result = engine.update(snap);

    REQUIRE(!result.explanation.top_drivers.empty());
    REQUIRE(result.explanation.top_drivers.size() <= 5);
}

// ============================================================
// Гистерезис
// ============================================================

TEST_CASE("WorldModel: гистерезис задерживает переход", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.hysteresis.confirmation_ticks = 3;
    config.hysteresis.min_dwell_ticks = 2;
    auto engine = make_engine_with_config(config);

    // Устанавливаем StableTrend
    auto snap_trend = make_trend_snapshot();
    for (int i = 0; i < 5; ++i) {
        engine.update(snap_trend);
    }

    auto state = engine.current_state(Symbol("BTCUSDT"));
    REQUIRE(state->state == WorldState::StableTrendContinuation);

    // Одиночный тик ChopNoise — гистерезис должен блокировать
    auto snap_chop = make_snapshot();
    snap_chop.technical.adx = 15.0;
    snap_chop.technical.adx_valid = true;
    snap_chop.technical.rsi_14 = 50.0;
    snap_chop.technical.rsi_valid = true;
    snap_chop.technical.sma_valid = true;
    snap_chop.microstructure.spread_valid = true;
    snap_chop.microstructure.spread_bps = 5.0;

    auto r1 = engine.update(snap_chop);
    // Гистерезис блокирует — остаёмся в StableTrend
    REQUIRE(r1.state == WorldState::StableTrendContinuation);
    REQUIRE(r1.explanation.hysteresis_overrode);
}

TEST_CASE("WorldModel: опасные состояния проходят без гистерезиса", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.hysteresis.confirmation_ticks = 5;
    auto engine = make_engine_with_config(config);

    // Устанавливаем StableTrend
    auto snap_trend = make_trend_snapshot();
    for (int i = 0; i < 5; ++i) engine.update(snap_trend);

    // LiquidityVacuum — проходит немедленно
    auto snap_lv = make_snapshot();
    snap_lv.technical.sma_valid = true;
    snap_lv.microstructure.spread_valid = true;
    snap_lv.microstructure.spread_bps = 60.0;
    auto result = engine.update(snap_lv);

    REQUIRE(result.state == WorldState::LiquidityVacuum);
}

// ============================================================
// История и transition
// ============================================================

TEST_CASE("WorldModel: dwell_ticks увеличивается при повторном состоянии", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();

    engine.update(snap);
    engine.update(snap);
    auto r3 = engine.update(snap);

    REQUIRE(r3.dwell_ticks >= 2);
}

TEST_CASE("WorldModel: persistence_score blend с эмпирическими данными", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.persistence.min_history_for_empirical = 3;
    config.persistence.history_blend_weight = 0.5;
    auto engine = make_engine_with_config(config);

    auto snap = make_trend_snapshot();
    // Много обновлений в одном состоянии → высокая эмпирическая персистентность
    for (int i = 0; i < 10; ++i) {
        engine.update(snap);
    }

    auto result = engine.update(snap);
    REQUIRE(result.persistence_score > 0.7);
}

// ============================================================
// State probabilities
// ============================================================

TEST_CASE("WorldModel: state_probabilities валидны", "[world_model]") {
    auto engine = make_engine();
    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);

    REQUIRE(result.state_probabilities.valid);

    double sum = 0.0;
    for (size_t i = 0; i < kWorldStateCount; ++i) {
        REQUIRE(result.state_probabilities.values[i] >= 0.0);
        sum += result.state_probabilities.values[i];
    }
    REQUIRE_THAT(sum, Catch::Matchers::WithinAbs(1.0, 0.01));

    // Основное состояние должно иметь наибольшую вероятность
    double primary_prob = result.state_probabilities.probability(WorldState::StableTrendContinuation);
    REQUIRE(primary_prob > 0.3);
}

// ============================================================
// Feedback
// ============================================================

TEST_CASE("WorldModel: feedback записывается и влияет на suitability", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.suitability.min_trades_for_feedback = 5;
    config.suitability.feedback_blend_weight = 0.5;
    auto engine = make_engine_with_config(config);

    // Записываем feedback: momentum в StableTrend постоянно проигрывает
    for (int i = 0; i < 10; ++i) {
        WorldStateFeedback fb;
        fb.state = WorldState::StableTrendContinuation;
        fb.strategy_id = StrategyId("momentum");
        fb.pnl_bps = -5.0;
        fb.was_profitable = false;
        fb.timestamp = Timestamp(1000000 + i);
        engine.record_feedback(fb);
    }

    auto stats = engine.performance_stats(
        WorldState::StableTrendContinuation, StrategyId("momentum"));
    REQUIRE(stats.has_value());
    REQUIRE(stats->total_trades == 10);
    REQUIRE(stats->ema_win_rate < 0.5);
}

// ============================================================
// model_version
// ============================================================

TEST_CASE("WorldModel: model_version из конфигурации", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    config.model_version = "3.0.0-test";
    auto engine = make_engine_with_config(config);

    REQUIRE(engine.model_version() == "3.0.0-test");

    auto snap = make_trend_snapshot();
    auto result = engine.update(snap);
    REQUIRE(result.model_version == "3.0.0-test");
}

// ============================================================
// Конфигурация: валидация
// ============================================================

TEST_CASE("WorldModel: config validation", "[world_model]") {
    auto config = WorldModelConfig::make_default();
    REQUIRE(config.validate());

    config.history.max_entries = 0;
    REQUIRE_FALSE(config.validate());
}

// ============================================================
// Multi-symbol
// ============================================================

TEST_CASE("WorldModel: multi-symbol изоляция", "[world_model]") {
    auto engine = make_engine();

    auto snap_btc = make_trend_snapshot();
    snap_btc.symbol = Symbol("BTCUSDT");
    engine.update(snap_btc);

    auto snap_eth = make_snapshot("ETHUSDT");
    snap_eth.technical.rsi_14 = 85.0;
    snap_eth.technical.rsi_valid = true;
    snap_eth.technical.momentum_5 = 0.05;
    snap_eth.technical.momentum_valid = true;
    snap_eth.technical.sma_valid = true;
    snap_eth.microstructure.spread_valid = true;
    snap_eth.microstructure.spread_bps = 5.0;
    engine.update(snap_eth);

    auto btc_state = engine.current_state(Symbol("BTCUSDT"));
    auto eth_state = engine.current_state(Symbol("ETHUSDT"));

    REQUIRE(btc_state->state == WorldState::StableTrendContinuation);
    REQUIRE(eth_state->state == WorldState::ExhaustionSpike);
}

// ============================================================
// Transition tendency
// ============================================================

TEST_CASE("WorldModel: transition tendency улучшение", "[world_model]") {
    auto engine = make_engine();

    // Сначала ExhaustionSpike (quality=-1)
    auto snap1 = make_snapshot();
    snap1.technical.rsi_14 = 85.0;
    snap1.technical.rsi_valid = true;
    snap1.technical.momentum_5 = 0.05;
    snap1.technical.momentum_valid = true;
    snap1.technical.sma_valid = true;
    snap1.microstructure.spread_valid = true;
    snap1.microstructure.spread_bps = 5.0;
    engine.update(snap1);

    // Затем PostShockStabilization → Improving
    auto snap2 = make_snapshot();
    snap2.technical.volatility_valid = true;
    snap2.technical.volatility_5 = 0.01;
    snap2.technical.volatility_20 = 0.03;
    snap2.technical.sma_valid = true;
    snap2.microstructure.spread_valid = true;
    snap2.microstructure.spread_bps = 5.0;
    auto r2 = engine.update(snap2);

    REQUIRE(r2.transition.tendency == TransitionTendency::Improving);
}

// ============================================================
// Composite fragility
// ============================================================

TEST_CASE("WorldModel: composite fragility учитывает микроструктуру", "[world_model]") {
    auto engine = make_engine();

    // Базовый ChopNoise (fragility ~0.3)
    auto snap = make_snapshot();
    snap.technical.adx = 15.0;
    snap.technical.adx_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    snap.technical.sma_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 5.0;
    auto r1 = engine.update(snap);
    double base_fragility = r1.fragility.value;

    // Тот же ChopNoise, но с высоким спредом → выше fragility
    snap.microstructure.spread_bps = 80.0;
    snap.microstructure.instability_valid = true;
    snap.microstructure.book_instability = 0.6;
    auto r2 = engine.update(snap);

    REQUIRE(r2.fragility.value > base_fragility);
}
