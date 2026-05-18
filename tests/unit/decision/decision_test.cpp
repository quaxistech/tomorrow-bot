#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_mocks.hpp"
#include "decision/decision_aggregation_engine.hpp"
#include "strategy/strategy_types.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include <memory>

using namespace tb;
using namespace tb::test;
using namespace tb::decision;
using namespace tb::strategy;
using namespace tb::regime;
using namespace tb::world_model;
using namespace tb::uncertainty;

namespace {

TradeIntent make_intent(const std::string& id, Side side, double conviction,
                        int64_t generated_at_ns = 1'000'000'000LL) {
    TradeIntent intent;
    intent.strategy_id = StrategyId(id);
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = Symbol("BTCUSDT");
    intent.side = side;
    intent.conviction = conviction;
    intent.signal_name = "test_signal";
    intent.generated_at = Timestamp(generated_at_ns);
    return intent;
}

AllocationResult make_allocation(const std::vector<std::pair<std::string, double>>& allocs) {
    AllocationResult result;
    for (const auto& [id, weight] : allocs) {
        StrategyAllocation a;
        a.strategy_id = StrategyId(id);
        a.is_enabled = weight > 0.0;
        a.weight = weight;
        a.size_multiplier = 1.0;
        result.allocations.push_back(std::move(a));
        if (weight > 0.0) ++result.enabled_count;
    }
    return result;
}

RegimeSnapshot make_regime() {
    RegimeSnapshot rs;
    rs.label = RegimeLabel::Trending;
    rs.detailed = DetailedRegime::StrongUptrend;
    rs.confidence = 0.8;
    return rs;
}

WorldModelSnapshot make_world() {
    WorldModelSnapshot ws;
    ws.state = WorldState::StableTrendContinuation;
    ws.label = WorldStateLabel::Stable;
    ws.fragility = {0.2, true};
    return ws;
}

UncertaintySnapshot make_low_uncertainty() {
    UncertaintySnapshot us;
    us.level = UncertaintyLevel::Low;
    us.aggregate_score = 0.15;
    us.recommended_action = UncertaintyAction::Normal;
    us.size_multiplier = 0.85;
    us.threshold_multiplier = 1.3;
    return us;
}

UncertaintySnapshot make_extreme_uncertainty() {
    UncertaintySnapshot us;
    us.level = UncertaintyLevel::Extreme;
    us.aggregate_score = 0.85;
    us.recommended_action = UncertaintyAction::NoTrade;
    us.size_multiplier = 0.15;
    us.threshold_multiplier = 2.7;
    return us;
}

} // anonymous namespace

TEST_CASE("Decision: один интент одобрен → trade_approved", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 0.7}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    // Debug output
    INFO("rationale=" << record.rationale);
    INFO("rejection=" << to_string(record.rejection_reason));
    INFO("conviction=" << record.final_conviction);
    INFO("threshold=" << record.effective_threshold);
    REQUIRE(record.trade_approved);
    REQUIRE(record.final_intent.has_value());
    REQUIRE(record.final_intent->side == Side::Buy);
    REQUIRE(record.final_conviction > 0.0);
    REQUIRE_FALSE(record.rationale.empty());
}

TEST_CASE("Decision: высокая неопределённость → глобальное вето", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 1.0}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_extreme_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
    REQUIRE_FALSE(record.global_vetoes.empty());
    REQUIRE(record.global_vetoes[0].source == "uncertainty");
}

// Removed in scalping refactor: BUY/SELL conflict resolution was retired
// because strategy_engine emits at most one intent per tick.

TEST_CASE("Decision: низкая conviction → не одобрен", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.1); // Очень низкая conviction
    auto allocation = make_allocation({{"momentum", 0.5}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
}

TEST_CASE("Decision: полный DecisionRecord содержит rationale", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 1.0}});

    auto record = engine.aggregate(
        Symbol("BTCUSDT"),
        {intent},
        allocation,
        make_regime(),
        make_world(),
        make_low_uncertainty());

    REQUIRE_FALSE(record.rationale.empty());
    REQUIRE(record.symbol.get() == "BTCUSDT");
    REQUIRE(record.is_reconstructable());
}

TEST_CASE("Decision: детерминизм — одинаковые входы → одинаковый результат", "[decision]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    CommitteeDecisionEngine engine(logger, clk);

    auto intent = make_intent("momentum", Side::Buy, 0.8);
    auto allocation = make_allocation({{"momentum", 0.7}});
    auto regime = make_regime();
    auto world = make_world();
    auto uncertainty = make_low_uncertainty();

    auto record1 = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation, regime, world, uncertainty);
    auto record2 = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation, regime, world, uncertainty);

    REQUIRE(record1.trade_approved == record2.trade_approved);
    REQUIRE(record1.final_conviction == record2.final_conviction);
}

// ============================================================================
// Расширенные тесты (профессиональные фичи)
// ============================================================================

namespace {

portfolio::PortfolioSnapshot make_portfolio(double dd_pct = 0.0, int consecutive = 0) {
    portfolio::PortfolioSnapshot snap;
    snap.total_capital = 10000.0;
    snap.available_capital = 9000.0;
    snap.pnl.current_drawdown_pct = dd_pct;
    snap.pnl.consecutive_losses = consecutive;
    snap.computed_at = Timestamp(1000000);
    return snap;
}

features::FeatureSnapshot make_features(double spread_bps = 5.0, double slippage_bps = 3.0) {
    features::FeatureSnapshot fs;
    fs.symbol = Symbol("BTCUSDT");
    fs.computed_at = Timestamp(1000000);
    fs.last_price = Price(50000.0);
    fs.mid_price = Price(50000.0);
    fs.microstructure.spread_bps = spread_bps;
    fs.microstructure.spread_valid = true;
    fs.execution_context.spread_cost_bps = spread_bps;
    fs.execution_context.estimated_slippage_bps = slippage_bps;
    fs.execution_context.slippage_valid = true;
    fs.technical.sma_valid = true;
    return fs;
}

RegimeSnapshot make_regime_with(DetailedRegime dr, double confidence = 0.8) {
    RegimeSnapshot rs;
    rs.label = RegimeLabel::Trending;
    rs.detailed = dr;
    rs.confidence = confidence;
    rs.computed_at = Timestamp(1000000);
    return rs;
}

} // anonymous namespace

TEST_CASE("Decision: time decay снижает conviction устаревших сигналов", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>(); // now = 1'000'000'000 ns (1 секунда)

    AdvancedDecisionConfig adv;
    adv.enable_time_decay = true;
    adv.time_decay_halflife_ms = 0.5; // 500_000 ns halflife → aggressive decay
    CommitteeDecisionEngine engine(logger, clk, 0.55, 0.65, adv);

    // Свежий сигнал (generated_at = now)
    auto fresh = make_intent("momentum", Side::Buy, 0.80, clk->current_time);

    // Устаревший сигнал (generated_at 1ms ago → 2 halflifes при halflife=0.5ms)
    auto stale = make_intent("momentum_stale", Side::Buy, 0.80,
                             clk->current_time - 1'000'000LL);

    auto allocation_fresh = make_allocation({{"momentum", 0.7}});
    auto allocation_stale = make_allocation({{"momentum_stale", 0.7}});

    auto rec_fresh = engine.aggregate(Symbol("BTCUSDT"), {fresh}, allocation_fresh,
        make_regime(), make_world(), make_low_uncertainty());
    auto rec_stale = engine.aggregate(Symbol("BTCUSDT"), {stale}, allocation_stale,
        make_regime(), make_world(), make_low_uncertainty());

    // Устаревший сигнал должен иметь более низкую conviction
    REQUIRE(rec_fresh.final_conviction > rec_stale.final_conviction);
}

// Removed in scalping refactor: regime-driven threshold multiplier was retired
// — uncertainty already incorporates regime confidence, and the legacy
// regime_choppy_factor double-counted that signal.

// Removed in scalping refactor: ensemble bonus path was retired with the
// single-strategy bot; voting is now a pass-through.

TEST_CASE("Decision: portfolio drawdown → повышенный порог conviction", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    AdvancedDecisionConfig adv;
    adv.enable_portfolio_awareness = true;
    adv.drawdown_boost_scale = 0.10;
    adv.consecutive_loss_boost = 0.03;
    // Отключаем другие эффекты для изоляции
    adv.enable_time_decay = false;
    adv.enable_execution_cost_modeling = false;
    // threshold=0.55, uncertainty.threshold_multiplier=1.3 → eff=0.715
    CommitteeDecisionEngine engine(logger, clk, 0.55, 0.65, adv);

    // conviction 0.75 > 0.715 → проходит без drawdown
    auto intent = make_intent("momentum", Side::Buy, 0.75);
    auto allocation = make_allocation({{"momentum", 0.7}});

    // Без просадки — проходит.
    auto portfolio_ok = make_portfolio(0.0, 0);
    auto rec_ok = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty(),
        portfolio_ok);
    REQUIRE(rec_ok.trade_approved);

    // Scalping refactor 2026-05: the threshold formula is now bounded
    // (base + max suppression 0.25 + dd_boost up to advanced_.drawdown_max_boost).
    // The pre-refactor expectation that an 8% drawdown blocks a 0.75-conviction
    // entry no longer holds — DD shrinks size via leverage/uncertainty rather
    // than gating the trade out entirely.
    auto portfolio_dd = make_portfolio(-8.0, 5);
    auto rec_dd = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty(),
        portfolio_dd);
    // We still assert the drawdown_threshold_boost rises with DD.
    REQUIRE(rec_dd.drawdown_threshold_boost >= rec_ok.drawdown_threshold_boost);
}

TEST_CASE("Decision: execution cost penalty снижает conviction", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    AdvancedDecisionConfig adv;
    adv.enable_execution_cost_modeling = true;
    adv.max_acceptable_cost_bps = 80.0;
    // Отключаем другие эффекты
    adv.enable_time_decay = false;
    adv.enable_portfolio_awareness = false;
    // threshold=0.50, unc_mult=1.3 → eff=0.65
    CommitteeDecisionEngine engine(logger, clk, 0.50, 0.65, adv);

    auto intent = make_intent("momentum", Side::Buy, 0.90);
    auto allocation = make_allocation({{"momentum", 0.7}});

    // Низкие издержки
    auto features_low = make_features(3.0, 2.0);
    auto rec_low = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty(),
        std::nullopt, features_low);

    // Высокие издержки
    auto features_high = make_features(40.0, 30.0);
    auto rec_high = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty(),
        std::nullopt, features_high);

    // Высокие издержки → conviction ниже
    REQUIRE(rec_low.final_conviction > rec_high.final_conviction);
    // Высокая стоимость должна быть отражена
    REQUIRE(rec_high.execution_cost.total_cost_bps > rec_low.execution_cost.total_cost_bps);
}

TEST_CASE("Decision: execution cost > max → veto", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    AdvancedDecisionConfig adv;
    adv.enable_execution_cost_modeling = true;
    adv.max_acceptable_cost_bps = 50.0; // Жёсткий лимит 50 bps
    adv.enable_time_decay = false;
    adv.enable_portfolio_awareness = false;
    CommitteeDecisionEngine engine(logger, clk, 0.55, 0.65, adv);

    auto intent = make_intent("momentum", Side::Buy, 0.90);
    auto allocation = make_allocation({{"momentum", 1.0}});

    // Суммарные издержки 80 bps > порог 50 bps
    auto features_expensive = make_features(50.0, 30.0);
    auto record = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty(),
        std::nullopt, features_expensive);

    REQUIRE_FALSE(record.trade_approved);
    bool has_exec_veto = false;
    for (const auto& v : record.global_vetoes) {
        if (v.source == "execution_cost") has_exec_veto = true;
    }
    REQUIRE(has_exec_veto);
}

TEST_CASE("Decision: structured rejection reason присутствует при отказе", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    AdvancedDecisionConfig adv;
    CommitteeDecisionEngine engine(logger, clk, 0.80, 0.65, adv); // Высокий порог

    auto intent = make_intent("momentum", Side::Buy, 0.60);
    auto allocation = make_allocation({{"momentum", 0.7}});

    auto record = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty());

    REQUIRE_FALSE(record.trade_approved);
    REQUIRE(record.rejection_reason != RejectionReason::None);
}

TEST_CASE("Decision: approval_gap положительный при одобрении", "[decision][advanced]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();

    AdvancedDecisionConfig adv;
    adv.enable_time_decay = false;
    CommitteeDecisionEngine engine(logger, clk, 0.55, 0.65, adv);

    auto intent = make_intent("momentum", Side::Buy, 0.85);
    auto allocation = make_allocation({{"momentum", 1.0}});

    auto record = engine.aggregate(Symbol("BTCUSDT"), {intent}, allocation,
        make_regime(), make_world(), make_low_uncertainty());

    REQUIRE(record.trade_approved);
    REQUIRE(record.approval_gap > 0.0);
}
