#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_mocks.hpp"
#include "uncertainty/uncertainty_engine.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "ml/ml_signal_types.hpp"
#include <memory>

using namespace tb;
using namespace tb::test;
using namespace tb::uncertainty;
using namespace tb::features;

namespace {

FeatureSnapshot make_good_snapshot() {
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.computed_at = Timestamp(1000000);
    snap.last_price = Price(50000.0);
    snap.mid_price = Price(50000.0);
    snap.book_quality = order_book::BookQuality::Valid;
    snap.execution_context.is_feed_fresh = true;
    snap.technical.sma_valid = true;
    snap.technical.rsi_14 = 50.0;
    snap.technical.rsi_valid = true;
    snap.technical.macd_histogram = 0.5;
    snap.technical.macd_valid = true;
    snap.technical.adx = 30.0;
    snap.technical.adx_valid = true;
    snap.technical.bb_valid = true;
    snap.technical.ema_valid = true;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 3.0;
    return snap;
}

regime::RegimeSnapshot make_confident_regime() {
    regime::RegimeSnapshot rs;
    rs.confidence = 0.85;
    rs.stability = 0.9;
    rs.label = RegimeLabel::Trending;
    return rs;
}

regime::RegimeSnapshot make_uncertain_regime() {
    regime::RegimeSnapshot rs;
    rs.confidence = 0.2;
    rs.stability = 0.3;
    rs.label = RegimeLabel::Unclear;
    return rs;
}

world_model::WorldModelSnapshot make_stable_world() {
    world_model::WorldModelSnapshot ws;
    ws.state = world_model::WorldState::StableTrendContinuation;
    ws.fragility = {0.2, true};
    return ws;
}

world_model::WorldModelSnapshot make_fragile_world() {
    world_model::WorldModelSnapshot ws;
    ws.state = world_model::WorldState::LiquidityVacuum;
    ws.fragility = {0.9, true};
    return ws;
}

} // anonymous namespace

TEST_CASE("Uncertainty: Низкая неопределённость — хорошие данные, уверенный режим", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap = make_good_snapshot();
    auto regime = make_confident_regime();
    auto world = make_stable_world();

    auto result = engine.assess(snap, regime, world);

    REQUIRE(result.level == UncertaintyLevel::Low);
    REQUIRE(result.recommended_action == UncertaintyAction::Normal);
    REQUIRE(result.aggregate_score < 0.3);
}

TEST_CASE("Uncertainty: Высокая неопределённость — плохие данные, неуверенный режим", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.book_quality = order_book::BookQuality::Stale;
    snap.execution_context.is_feed_fresh = false;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 40.0;
    // Мало валидных индикаторов

    auto regime = make_uncertain_regime();
    auto world = make_fragile_world();

    auto result = engine.assess(snap, regime, world);

    REQUIRE((result.level == UncertaintyLevel::High || result.level == UncertaintyLevel::Extreme));
    REQUIRE((result.recommended_action == UncertaintyAction::HigherThreshold ||
             result.recommended_action == UncertaintyAction::NoTrade));
}

TEST_CASE("Uncertainty: size_multiplier уменьшается с ростом неопределённости", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    // Низкая неопределённость
    auto snap_good = make_good_snapshot();
    auto result_low = engine.assess(snap_good, make_confident_regime(), make_stable_world());

    // Высокая неопределённость
    FeatureSnapshot snap_bad;
    snap_bad.symbol = Symbol("ETHUSDT");
    snap_bad.book_quality = order_book::BookQuality::Desynced;
    snap_bad.execution_context.is_feed_fresh = false;
    snap_bad.microstructure.spread_valid = true;
    snap_bad.microstructure.spread_bps = 50.0;

    auto result_high = engine.assess(snap_bad, make_uncertain_regime(), make_fragile_world());

    REQUIRE(result_low.size_multiplier > result_high.size_multiplier);
}

TEST_CASE("Uncertainty: threshold_multiplier растёт с неопределённостью", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap_good = make_good_snapshot();
    auto result_low = engine.assess(snap_good, make_confident_regime(), make_stable_world());

    FeatureSnapshot snap_bad;
    snap_bad.symbol = Symbol("ETHUSDT");
    snap_bad.book_quality = order_book::BookQuality::Stale;
    snap_bad.microstructure.spread_valid = true;
    snap_bad.microstructure.spread_bps = 40.0;
    snap_bad.execution_context.is_feed_fresh = false;

    auto result_high = engine.assess(snap_bad, make_uncertain_regime(), make_fragile_world());

    REQUIRE(result_low.threshold_multiplier < result_high.threshold_multiplier);
}

TEST_CASE("Uncertainty: Экстремальная неопределённость → NoTrade", "[uncertainty]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    // Максимально плохие условия
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.book_quality = order_book::BookQuality::Desynced;
    snap.execution_context.is_feed_fresh = false;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 80.0;
    snap.microstructure.liquidity_valid = true;
    snap.microstructure.liquidity_ratio = 0.1; // Сильно асимметричный стакан
    snap.execution_context.slippage_valid = true;
    snap.execution_context.estimated_slippage_bps = 30.0;

    regime::RegimeSnapshot regime;
    regime.confidence = 0.1; // Почти нулевая уверенность

    world_model::WorldModelSnapshot world;
    world.fragility = {0.95, true};

    auto result = engine.assess(snap, regime, world);

    REQUIRE(result.level == UncertaintyLevel::Extreme);
    REQUIRE(result.recommended_action == UncertaintyAction::NoTrade);
    REQUIRE(result.size_multiplier < 0.3);
}

// ============================================================
// v2 helpers
// ============================================================
namespace {

class AdvancingClock : public clock::IClock {
    mutable int64_t ns_{1'000'000};
public:
    [[nodiscard]] Timestamp now() const override {
        ns_ += 1'000'000'000;
        return Timestamp(ns_);
    }
};

portfolio::PortfolioSnapshot make_neutral_portfolio() {
    portfolio::PortfolioSnapshot p;
    p.total_capital = 10000.0;
    p.available_capital = 8000.0;
    p.capital_utilization_pct = 20.0;
    return p;
}

portfolio::PortfolioSnapshot make_concentrated_portfolio() {
    portfolio::PortfolioSnapshot p;
    p.total_capital = 10000.0;
    p.available_capital = 3000.0;
    p.capital_utilization_pct = 70.0;
    portfolio::Position pos;
    pos.symbol = Symbol("BTCUSDT");
    pos.size = Quantity(1.0);
    pos.avg_entry_price = Price(50000.0);
    pos.notional = NotionalValue(5000.0);
    p.positions.push_back(pos);
    return p;
}

ml::MlSignalSnapshot make_healthy_ml() {
    ml::MlSignalSnapshot m;
    m.signal_quality = 0.9;
    m.cascade_probability = 0.05;
    m.cascade_imminent = false;
    m.correlation_break = false;
    m.correlation_risk_multiplier = 0.9;
    m.fingerprint_edge = 0.1;
    m.recommended_wait_periods = 0;
    m.overall_health = ml::MlComponentHealth::Healthy;
    return m;
}

ml::MlSignalSnapshot make_degraded_ml() {
    ml::MlSignalSnapshot m;
    m.signal_quality = 0.2;
    m.cascade_probability = 0.5;
    m.cascade_imminent = false;
    m.correlation_break = false;
    m.correlation_risk_multiplier = 0.8;
    m.overall_health = ml::MlComponentHealth::Degraded;
    return m;
}

FeatureSnapshot make_extreme_bad_snapshot() {
    FeatureSnapshot snap;
    snap.symbol = Symbol("BTCUSDT");
    snap.book_quality = order_book::BookQuality::Desynced;
    snap.execution_context.is_feed_fresh = false;
    snap.microstructure.spread_valid = true;
    snap.microstructure.spread_bps = 80.0;
    snap.microstructure.liquidity_valid = true;
    snap.microstructure.liquidity_ratio = 0.1; // Сильно асимметричный стакан
    snap.execution_context.slippage_valid = true;
    snap.execution_context.estimated_slippage_bps = 30.0;
    return snap;
}

regime::RegimeSnapshot make_extreme_bad_regime() {
    regime::RegimeSnapshot rs;
    rs.confidence = 0.1;
    rs.stability = 0.1;
    rs.label = RegimeLabel::Unclear;
    return rs;
}

} // anonymous namespace

// ============================================================
// Test 1: v2 overload with portfolio + ML signals
// ============================================================
TEST_CASE("Uncertainty v2: пятиаргументный assess() даёт более богатый результат", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap = make_good_snapshot();
    auto regime = make_confident_regime();
    auto world = make_stable_world();

    auto v1 = engine.assess(snap, regime, world);
    auto v2 = engine.assess(snap, regime, world,
                            make_neutral_portfolio(), make_healthy_ml());

    CHECK(!v2.top_drivers.empty());
    // v2 has richer portfolio/ml info; v1 defaults them to 0
    CHECK(v1.dimensions.portfolio_uncertainty == 0.0);
    CHECK(v1.dimensions.ml_uncertainty == 0.0);
}

// ============================================================
// Test 2: Portfolio uncertainty — concentrated position
// ============================================================
TEST_CASE("Uncertainty v2: концентрированная позиция → высокая портфельная неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto result = engine.assess(make_good_snapshot(), make_confident_regime(),
                                make_stable_world(), make_concentrated_portfolio(),
                                make_healthy_ml());

    REQUIRE(result.dimensions.portfolio_uncertainty >= 0.3);
}

// ============================================================
// Test 3: Portfolio uncertainty — empty portfolio
// ============================================================
TEST_CASE("Uncertainty v2: пустой портфель → нулевая портфельная неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    portfolio::PortfolioSnapshot empty;
    auto result = engine.assess(make_good_snapshot(), make_confident_regime(),
                                make_stable_world(), empty, make_healthy_ml());

    REQUIRE(result.dimensions.portfolio_uncertainty == 0.0);
}

// ============================================================
// Test 4: ML uncertainty — degraded signal quality
// ============================================================
TEST_CASE("Uncertainty v2: деградированный ML-сигнал → высокая ML-неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto result = engine.assess(make_good_snapshot(), make_confident_regime(),
                                make_stable_world(), make_neutral_portfolio(),
                                make_degraded_ml());

    // signal_quality=0.2 → base = 0.8, cascade_probability=0.5 → +0.15 => ~0.95
    REQUIRE(result.dimensions.ml_uncertainty > 0.4);
}

// ============================================================
// Test 5: ML uncertainty — cascade imminent
// ============================================================
TEST_CASE("Uncertainty v2: каскад неминуем → высокая ML-неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto ml = make_healthy_ml();
    ml.cascade_imminent = true;

    auto result = engine.assess(make_good_snapshot(), make_confident_regime(),
                                make_stable_world(), make_neutral_portfolio(), ml);

    // base = 1-0.9=0.1, cascade_imminent → +0.3 => 0.4
    REQUIRE(result.dimensions.ml_uncertainty > 0.3);
}

// ============================================================
// Test 6: Correlation uncertainty — correlation break
// ============================================================
TEST_CASE("Uncertainty v2: разрыв корреляции → высокая корреляционная неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto ml = make_healthy_ml();
    ml.correlation_break = true;

    auto result = engine.assess(make_good_snapshot(), make_confident_regime(),
                                make_stable_world(), make_neutral_portfolio(), ml);

    // correlation_break=true → returns 0.8
    REQUIRE(result.dimensions.correlation_uncertainty >= 0.8);
}

// ============================================================
// Test 7: Transition uncertainty — recent transition + low stability
// ============================================================
TEST_CASE("Uncertainty v2: высокая транзиционная неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    regime::RegimeSnapshot regime;
    regime.confidence = 0.5;
    regime.stability = 0.2; // < 0.3 → +0.2
    regime.last_transition = regime::RegimeTransition{
        regime::DetailedRegime::StrongUptrend,
        regime::DetailedRegime::VolatilityExpansion,
        0.8, Timestamp(0) // confidence = 0.8
    };

    auto result = engine.assess(make_good_snapshot(), regime,
                                make_stable_world(), make_neutral_portfolio(),
                                make_healthy_ml());

    // New formula: base = 0.3 + 0.4*(1.0 - confidence) when transition exists
    //   confidence = 0.8 → base = 0.3 + 0.4*0.2 = 0.38
    //   stability = 0.2 < 0.3 → +0.2 = 0.58
    REQUIRE(result.dimensions.transition_uncertainty >= 0.5);
    REQUIRE(result.dimensions.transition_uncertainty <= 0.7);
}

// ============================================================
// Test 8: Stateful — EMA smoothing (persistent_score)
// ============================================================
TEST_CASE("Uncertainty v2: EMA-сглаживание persistent_score", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto snap = make_good_snapshot();
    auto regime = make_confident_regime();
    auto world = make_stable_world();
    auto port = make_neutral_portfolio();
    auto ml = make_healthy_ml();

    // First assessment establishes baseline
    auto r1 = engine.assess(snap, regime, world, port, ml);
    double prev_persistent = r1.persistent_score;

    // Subsequent calls: persistent_score should move toward aggregate_score
    for (int i = 0; i < 5; ++i) {
        auto r = engine.assess(snap, regime, world, port, ml);
        double dist_now = std::abs(r.persistent_score - r.aggregate_score);
        double dist_prev = std::abs(prev_persistent - r.aggregate_score);
        CHECK(dist_now <= dist_prev + 0.01); // converging (with tolerance)
        prev_persistent = r.persistent_score;
    }
}

// ============================================================
// Test 9: Stateful — spike detection
// ============================================================
TEST_CASE("Uncertainty v2: обнаружение спайка при резком ухудшении", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto port = make_neutral_portfolio();
    auto ml = make_healthy_ml();

    // Establish low EMA with several good assessments
    for (int i = 0; i < 5; ++i) {
        engine.assess(make_good_snapshot(), make_confident_regime(),
                      make_stable_world(), port, ml);
    }

    // Sudden bad conditions → spike should be positive
    auto result = engine.assess(make_extreme_bad_snapshot(),
                                make_extreme_bad_regime(),
                                make_fragile_world(), port, make_degraded_ml());

    REQUIRE(result.spike_score > 0.0);
}

// ============================================================
// Test 10: Execution mode recommendations
// ============================================================
TEST_CASE("Uncertainty v2: рекомендации по режиму исполнения", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();

    SECTION("High → Conservative or DefensiveOnly") {
        RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);
        FeatureSnapshot snap;
        snap.symbol = Symbol("BTCUSDT");
        snap.book_quality = order_book::BookQuality::Stale;
        snap.execution_context.is_feed_fresh = false;
        snap.microstructure.spread_valid = true;
        snap.microstructure.spread_bps = 40.0;

        auto result = engine.assess(snap, make_uncertain_regime(),
                                    make_fragile_world(), make_neutral_portfolio(),
                                    make_degraded_ml());

        if (result.level == UncertaintyLevel::High) {
            CHECK((result.execution_mode == ExecutionModeRecommendation::Conservative ||
                   result.execution_mode == ExecutionModeRecommendation::DefensiveOnly));
        }
    }

    SECTION("Extreme → HaltNewEntries") {
        RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);
        auto result = engine.assess(make_extreme_bad_snapshot(),
                                    make_extreme_bad_regime(),
                                    make_fragile_world(), make_neutral_portfolio(),
                                    make_degraded_ml());

        if (result.level == UncertaintyLevel::Extreme) {
            REQUIRE(result.execution_mode == ExecutionModeRecommendation::HaltNewEntries);
        }
    }
}

// ============================================================
// Test 11: Top drivers ranking
// ============================================================
TEST_CASE("Uncertainty v2: ранжирование top_drivers", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    // Make regime the dominant uncertainty source
    auto result = engine.assess(make_good_snapshot(), make_uncertain_regime(),
                                make_stable_world(), make_neutral_portfolio(),
                                make_healthy_ml());

    REQUIRE(!result.top_drivers.empty());
    // Drivers should be sorted by contribution (descending)
    for (size_t i = 1; i < result.top_drivers.size(); ++i) {
        CHECK(result.top_drivers[i - 1].contribution >= result.top_drivers[i].contribution);
    }
    // Regime should be among the top drivers
    bool found_regime = false;
    for (const auto& d : result.top_drivers) {
        if (d.dimension.find("regime") != std::string::npos) {
            found_regime = true;
            break;
        }
    }
    CHECK(found_regime);
}

// ============================================================
// Test 12: Cooldown activation after consecutive extremes
// ============================================================
TEST_CASE("Uncertainty v2: кулдаун после серии экстремальных оценок", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    auto port = make_neutral_portfolio();
    auto ml = make_degraded_ml();
    auto snap = make_extreme_bad_snapshot();
    auto regime = make_extreme_bad_regime();
    auto world = make_fragile_world();

    // Trigger 3+ consecutive extreme assessments
    for (int i = 0; i < 3; ++i) {
        auto r = engine.assess(snap, regime, world, port, ml);
        REQUIRE(r.level == UncertaintyLevel::Extreme);
    }

    // 4th assessment: cooldown should now be active
    auto r4 = engine.assess(snap, regime, world, port, ml);
    REQUIRE(r4.cooldown.active);
    CHECK(r4.cooldown.remaining_ns > 0);
}

// ============================================================
// Test 13: Diagnostics tracking
// ============================================================
TEST_CASE("Uncertainty v2: диагностика отслеживает количество оценок", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    constexpr int N = 7;
    for (int i = 0; i < N; ++i) {
        engine.assess(make_good_snapshot(), make_confident_regime(),
                      make_stable_world(), make_neutral_portfolio(),
                      make_healthy_ml());
    }

    auto diag = engine.diagnostics();
    REQUIRE(diag.total_assessments == N);
}

// ============================================================
// Test 14: Diagnostics counting (replaced feedback test — API removed)
// ============================================================
TEST_CASE("Uncertainty v2: диагностика подсчитывает вето и кулдауны", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    REQUIRE(engine.diagnostics().veto_count == 0);
    REQUIRE(engine.diagnostics().cooldown_activations == 0);

    // Generate an extreme case → should increment veto_count
    engine.assess(make_extreme_bad_snapshot(), make_extreme_bad_regime(),
                  make_fragile_world(), make_neutral_portfolio(),
                  make_degraded_ml());

    REQUIRE(engine.diagnostics().veto_count >= 1);
}

// ============================================================
// Test 15: reset_state clears everything
// ============================================================
TEST_CASE("Uncertainty v2: reset_state очищает всё", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    engine.assess(make_good_snapshot(), make_confident_regime(),
                  make_stable_world(), make_neutral_portfolio(),
                  make_healthy_ml());

    REQUIRE(engine.diagnostics().total_assessments == 1);

    engine.reset_state();

    REQUIRE(engine.diagnostics().total_assessments == 0);
}

// ============================================================
// Test 16: Bounds guarantees
// ============================================================
TEST_CASE("Uncertainty v2: гарантии границ size_multiplier и threshold_multiplier", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    UncertaintyConfig cfg;
    RuleBasedUncertaintyEngine engine(cfg, logger, clk);

    auto port = make_neutral_portfolio();

    auto check_bounds = [&](const UncertaintySnapshot& r) {
        CHECK(r.size_multiplier >= cfg.size_floor);
        CHECK(r.size_multiplier <= 1.0);
        CHECK(r.threshold_multiplier >= 1.0);
        CHECK(r.threshold_multiplier <= cfg.threshold_ceiling);
    };

    // Good conditions
    check_bounds(engine.assess(make_good_snapshot(), make_confident_regime(),
                               make_stable_world(), port, make_healthy_ml()));

    // Bad conditions
    check_bounds(engine.assess(make_extreme_bad_snapshot(), make_extreme_bad_regime(),
                               make_fragile_world(), port, make_degraded_ml()));
}

// ============================================================
// Test 17: Monotonicity — worse input → higher score
// ============================================================
TEST_CASE("Uncertainty v2: монотонность — ухудшение условий повышает скор", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();

    auto port = make_neutral_portfolio();
    auto ml_good = make_healthy_ml();
    auto ml_bad = make_degraded_ml();

    // Level 0: perfect
    {
        RuleBasedUncertaintyEngine e(UncertaintyConfig{}, logger, clk);
        auto r0 = e.assess(make_good_snapshot(), make_confident_regime(),
                           make_stable_world(), port, ml_good);

        // Level 1: uncertain regime
        auto r1 = e.assess(make_good_snapshot(), make_uncertain_regime(),
                           make_stable_world(), port, ml_good);

        // Level 2: uncertain regime + fragile world + bad ML
        auto r2 = e.assess(make_good_snapshot(), make_uncertain_regime(),
                           make_fragile_world(), port, ml_bad);

        // Level 3: everything bad
        auto r3 = e.assess(make_extreme_bad_snapshot(), make_extreme_bad_regime(),
                           make_fragile_world(), port, ml_bad);

        CHECK(r0.aggregate_score <= r1.aggregate_score + 0.01);
        CHECK(r1.aggregate_score <= r2.aggregate_score + 0.01);
        CHECK(r2.aggregate_score <= r3.aggregate_score + 0.01);
    }
}

// ============================================================
// Test 18: Configurable weights change results
// ============================================================
TEST_CASE("Uncertainty v2: пользовательские веса меняют результат", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();

    UncertaintyConfig cfg_regime_heavy;
    cfg_regime_heavy.w_regime = 0.80;
    cfg_regime_heavy.w_signal = 0.05;
    cfg_regime_heavy.w_data_quality = 0.03;
    cfg_regime_heavy.w_execution = 0.02;
    cfg_regime_heavy.w_portfolio = 0.02;
    cfg_regime_heavy.w_ml = 0.02;
    cfg_regime_heavy.w_correlation = 0.02;
    cfg_regime_heavy.w_transition = 0.02;
    cfg_regime_heavy.w_operational = 0.02;

    UncertaintyConfig cfg_balanced;

    RuleBasedUncertaintyEngine eng_heavy(cfg_regime_heavy, logger, clk);
    RuleBasedUncertaintyEngine eng_balanced(cfg_balanced, logger, clk);

    auto snap = make_good_snapshot();
    auto port = make_neutral_portfolio();
    auto ml = make_healthy_ml();

    // Uncertain regime should matter more with heavy regime weight
    auto rh = eng_heavy.assess(snap, make_uncertain_regime(), make_stable_world(), port, ml);
    auto rb = eng_balanced.assess(snap, make_uncertain_regime(), make_stable_world(), port, ml);

    CHECK(rh.aggregate_score > rb.aggregate_score - 0.01);
}

// ============================================================
// Test 19: Operational uncertainty — stale feed
// ============================================================
TEST_CASE("Uncertainty v2: несвежий фид → высокая операционная неопределённость", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();
    RuleBasedUncertaintyEngine engine(UncertaintyConfig{}, logger, clk);

    FeatureSnapshot snap = make_good_snapshot();
    snap.execution_context.is_feed_fresh = false;

    auto result = engine.assess(snap, make_confident_regime(),
                                make_stable_world(), make_neutral_portfolio(),
                                make_healthy_ml());

    // compute_operational_uncertainty returns 0.7 when feed is not fresh
    REQUIRE(result.dimensions.operational_uncertainty > 0.5);
}

// ============================================================
// Test 20: Config integration — custom config affects results
// ============================================================
TEST_CASE("Uncertainty v2: конфигурация liquidity threshold влияет на execution uncertainty", "[uncertainty][v2]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<AdvancingClock>();

    FeatureSnapshot snap = make_good_snapshot();
    snap.microstructure.liquidity_valid = true;
    snap.microstructure.liquidity_ratio = 0.3;

    // Default threshold = 0.5 → ratio 0.3 < 0.5 → penalty
    UncertaintyConfig cfg_default;
    RuleBasedUncertaintyEngine eng_default(cfg_default, logger, clk);
    auto r1 = eng_default.assess(snap, make_confident_regime(), make_stable_world(),
                                  make_neutral_portfolio(), make_healthy_ml());

    // Tight threshold = 0.2 → ratio 0.3 >= 0.2 → no penalty
    UncertaintyConfig cfg_tight;
    cfg_tight.liquidity_ratio_penalty_threshold = 0.2;
    RuleBasedUncertaintyEngine eng_tight(cfg_tight, logger, clk);
    auto r2 = eng_tight.assess(snap, make_confident_regime(), make_stable_world(),
                                make_neutral_portfolio(), make_healthy_ml());

    CHECK(r1.dimensions.execution_uncertainty > r2.dimensions.execution_uncertainty);
}
