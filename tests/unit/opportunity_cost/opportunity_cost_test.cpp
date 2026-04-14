#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "test_mocks.hpp"
#include "opportunity_cost/opportunity_cost_engine.hpp"

using namespace tb;
using namespace tb::test;
using namespace tb::opportunity_cost;
using namespace Catch::Matchers;

// ========== Вспомогательные функции ==========

static strategy::TradeIntent make_intent(double conviction = 0.7, double urgency = 0.5) {
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("momentum_v1");
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.suggested_quantity = Quantity(0.1);
    intent.conviction = conviction;
    intent.urgency = urgency;
    intent.correlation_id = CorrelationId("test-corr-1");
    return intent;
}

static execution_alpha::ExecutionAlphaResult make_exec_alpha(double total_cost_bps = 5.0) {
    execution_alpha::ExecutionAlphaResult result;
    result.recommended_style = execution_alpha::ExecutionStyle::Passive;
    result.quality.total_cost_bps = total_cost_bps;
    result.quality.spread_cost_bps = 3.0;
    result.quality.estimated_slippage_bps = 2.0;
    result.should_execute = true;
    return result;
}

static PortfolioContext make_portfolio(double exposure = 0.2) {
    PortfolioContext ctx;
    ctx.gross_exposure_pct = exposure;
    ctx.current_drawdown_pct = 0.0;
    ctx.consecutive_losses = 0;
    ctx.symbol_exposure_pct = 0.0;
    ctx.strategy_exposure_pct = 0.0;
    return ctx;
}

static RuleBasedOpportunityCost make_engine() {
    OpportunityCostConfig cfg;
    return RuleBasedOpportunityCost(
        std::move(cfg),
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>());
}

static RuleBasedOpportunityCost make_engine_with_config(OpportunityCostConfig cfg) {
    return RuleBasedOpportunityCost(
        std::move(cfg),
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>());
}

// ========== Тесты: базовые решения ==========

TEST_CASE("OpportunityCost: Высокая conviction, низкая стоимость → Execute", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.8);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Execute);
    REQUIRE(result.reason == OpportunityReason::StrongEdgeAvailable);
    REQUIRE(result.score.net_expected_bps > 0.0);
    REQUIRE(result.factors.rule_id == 8);
}

TEST_CASE("OpportunityCost: Низкая conviction ниже порога → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.1);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Suppress);
    // conviction 0.1 → expected_return ≈ 3.2 bps (0.1^1.5 * 100), minus exec cost 5.0 → net < min_net → Rule 1
    REQUIRE(result.reason == OpportunityReason::NegativeNetEdge);
}

TEST_CASE("OpportunityCost: Высокая экспозиция при средней conviction → Defer", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.85);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Defer);
    // exposure 0.85 > high_exposure_threshold 0.70 AND conviction 0.5 < min_conviction 0.60
    REQUIRE(result.reason == OpportunityReason::HighExposureLowConviction);
}

TEST_CASE("OpportunityCost: Отрицательный чистый доход → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.01);
    auto exec_alpha = make_exec_alpha(50.0);
    auto portfolio = make_portfolio(0.2);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.0);

    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.reason == OpportunityReason::NegativeNetEdge);
    REQUIRE(result.score.net_expected_bps < 0.0);
}

TEST_CASE("OpportunityCost: to_string работает корректно", "[opportunity_cost]") {
    REQUIRE(to_string(OpportunityAction::Execute) == "Execute");
    REQUIRE(to_string(OpportunityAction::Defer) == "Defer");
    REQUIRE(to_string(OpportunityAction::Suppress) == "Suppress");
    REQUIRE(to_string(OpportunityAction::Upgrade) == "Upgrade");
}

// ========== Тесты: новая функциональность ==========

TEST_CASE("OpportunityCost: to_string(OpportunityReason) корректен", "[opportunity_cost]") {
    REQUIRE(to_string(OpportunityReason::NegativeNetEdge) == "NegativeNetEdge");
    REQUIRE(to_string(OpportunityReason::StrongEdgeAvailable) == "StrongEdgeAvailable");
    REQUIRE(to_string(OpportunityReason::UpgradeBetterCandidate) == "UpgradeBetterCandidate");
    REQUIRE(to_string(OpportunityReason::DefaultDefer) == "DefaultDefer");
}

TEST_CASE("OpportunityCost: Капитал исчерпан → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.9);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.95);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.reason == OpportunityReason::CapitalExhausted);
}

TEST_CASE("OpportunityCost: Высокая концентрация символа → Suppress", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.8);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.3);
    portfolio.symbol_exposure_pct = 0.30;  // > 0.25 default max

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.reason == OpportunityReason::HighConcentration);
}

TEST_CASE("OpportunityCost: Высокая концентрация стратегии → Defer", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.8);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.3);
    portfolio.strategy_exposure_pct = 0.40;  // > 0.35 default max

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Defer);
    REQUIRE(result.reason == OpportunityReason::HighConcentration);
}

TEST_CASE("OpportunityCost: Upgrade — кандидат лучше худшей позиции", "[opportunity_cost]") {
    OpportunityCostConfig cfg;
    cfg.high_exposure_threshold = 0.50;  // Сделаем порог ниже
    auto engine = make_engine_with_config(std::move(cfg));
    auto intent = make_intent(0.8);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.60);
    portfolio.has_worst_position = true;
    portfolio.worst_position_net_bps = 5.0;

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // net_expected_bps значительно больше 5.0 + 10.0 (upgrade threshold)
    REQUIRE(result.action == OpportunityAction::Upgrade);
    REQUIRE(result.reason == OpportunityReason::UpgradeBetterCandidate);
}

TEST_CASE("OpportunityCost: HighConvictionOverride при высокой экспозиции", "[opportunity_cost]") {
    OpportunityCostConfig cfg;
    cfg.execute_min_net_bps = 200.0;  // Увысим порог чтобы Rule 8 не сработал
    auto engine = make_engine_with_config(std::move(cfg));
    auto intent = make_intent(0.9);  // Очень высокий conviction
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.60);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.action == OpportunityAction::Execute);
    REQUIRE(result.reason == OpportunityReason::HighConvictionOverride);
}

TEST_CASE("OpportunityCost: Drawdown повышает effective threshold", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.35);  // Чуть выше базового порога 0.3
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);
    portfolio.current_drawdown_pct = 0.10;  // 10% drawdown (fraction) → +1.0 к порогу

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // Effective threshold = 0.3 + 0.5*(0.10/0.05) = 1.3, clamped to 0.95
    // conviction 0.35 < 0.95 → Suppress
    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.reason == OpportunityReason::ConvictionBelowThreshold);
}

TEST_CASE("OpportunityCost: Consecutive losses повышают threshold", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.35);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);
    portfolio.consecutive_losses = 5;  // +0.10 к порогу

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // Effective threshold = 0.3 + 0.02*5 = 0.40 > 0.35
    REQUIRE(result.action == OpportunityAction::Suppress);
    REQUIRE(result.reason == OpportunityReason::ConvictionBelowThreshold);
}

TEST_CASE("OpportunityCost: Score decomposition корректна", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.7, 0.6);
    auto exec_alpha = make_exec_alpha(10.0);
    auto portfolio = make_portfolio(0.3);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // Проверяем, что все компоненты score заполнены
    REQUIRE(result.score.expected_return_bps > 0.0);
    REQUIRE(result.score.execution_cost_bps == 10.0);
    REQUIRE(result.score.net_expected_bps == result.score.expected_return_bps - 10.0);
    REQUIRE(result.score.score > 0.0);
    REQUIRE(result.score.score <= 1.0);
    REQUIRE(result.score.conviction_component > 0.0);
    REQUIRE(result.score.net_edge_component >= 0.0);
    REQUIRE(result.score.urgency_component > 0.0);
}

TEST_CASE("OpportunityCost: Factors audit trail заполнена", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.8);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    REQUIRE(result.factors.input_conviction == 0.8);
    REQUIRE(result.factors.input_execution_cost_bps == 5.0);
    REQUIRE(result.factors.input_exposure_pct == 0.2);
    REQUIRE(result.factors.rule_id > 0);
    REQUIRE(!result.reason_codes.empty());
    REQUIRE(!result.rationale.empty());
}

TEST_CASE("OpportunityCost: Counterfactual заполнен", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.5);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.85);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // При высокой экспозиции → Defer, но without exposure → мог бы быть Execute
    REQUIRE(result.action == OpportunityAction::Defer);
    REQUIRE(result.factors.would_be_without_exposure != OpportunityAction::Suppress);
}

TEST_CASE("OpportunityCost: Monotonicity — более высокий conviction → лучший score", "[opportunity_cost]") {
    auto engine = make_engine();
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto r1 = engine.evaluate(make_intent(0.3), exec_alpha, portfolio, 0.2);
    auto r2 = engine.evaluate(make_intent(0.5), exec_alpha, portfolio, 0.2);
    auto r3 = engine.evaluate(make_intent(0.8), exec_alpha, portfolio, 0.2);

    REQUIRE(r1.score.score < r2.score.score);
    REQUIRE(r2.score.score < r3.score.score);
}

TEST_CASE("OpportunityCost: NaN/Inf safety — нулевой capital", "[opportunity_cost]") {
    auto engine = make_engine();
    auto intent = make_intent(0.7);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.0);  // Нулевая экспозиция

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // Не должно быть NaN или Inf
    REQUIRE(std::isfinite(result.score.score));
    REQUIRE(std::isfinite(result.score.capital_efficiency));
    REQUIRE(std::isfinite(result.score.net_expected_bps));
}

TEST_CASE("OpportunityCost: Default Defer при пограничном edge", "[opportunity_cost]") {
    OpportunityCostConfig cfg;
    cfg.execute_min_net_bps = 200.0;        // Высокий порог
    cfg.high_exposure_min_conviction = 0.99; // Почти невозможно пройти
    auto engine = make_engine_with_config(std::move(cfg));
    auto intent = make_intent(0.5);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto result = engine.evaluate(intent, exec_alpha, portfolio, 0.3);

    // net_expected > 0, но < 200 → не Execute, conviction < 0.99 → не HighConviction
    REQUIRE(result.action == OpportunityAction::Defer);
    REQUIRE(result.reason == OpportunityReason::InsufficientNetEdge);
}

TEST_CASE("OpportunityCost: Configurable weights affect score", "[opportunity_cost]") {
    OpportunityCostConfig cfg1;
    cfg1.weight_conviction = 1.0;
    cfg1.weight_net_edge = 0.0;
    cfg1.weight_capital_efficiency = 0.0;
    cfg1.weight_urgency = 0.0;

    OpportunityCostConfig cfg2;
    cfg2.weight_conviction = 0.0;
    cfg2.weight_net_edge = 1.0;
    cfg2.weight_capital_efficiency = 0.0;
    cfg2.weight_urgency = 0.0;

    auto engine1 = make_engine_with_config(std::move(cfg1));
    auto engine2 = make_engine_with_config(std::move(cfg2));

    auto intent = make_intent(0.9);
    auto exec_alpha = make_exec_alpha(5.0);
    auto portfolio = make_portfolio(0.2);

    auto r1 = engine1.evaluate(intent, exec_alpha, portfolio, 0.3);
    auto r2 = engine2.evaluate(intent, exec_alpha, portfolio, 0.3);

    // Разные веса → разные score (не обязательно сильно, но не равны)
    // conviction_component и net_edge_component отличаются
    REQUIRE(r1.score.conviction_component == r2.score.conviction_component);
    REQUIRE(r1.score.score != r2.score.score);
}
