#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_mocks.hpp"
#include "execution/execution_engine.hpp"
#include "execution/order_submitter.hpp"
#include "execution/order_fsm.hpp"
#include "execution/order_types.hpp"
#include "execution/execution_types.hpp"
#include "execution/execution_config.hpp"
#include "execution/orders/order_registry.hpp"
#include "execution/orders/client_order_id.hpp"
#include "execution/planner/execution_planner.hpp"
#include "execution/fills/fill_processor.hpp"
#include "execution/cancel/cancel_manager.hpp"
#include "execution/recovery/recovery_manager.hpp"
#include "execution/telemetry/execution_metrics.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "uncertainty/uncertainty_types.hpp"

using namespace tb;
using namespace tb::test;
using namespace tb::execution;
using namespace Catch::Matchers;

// ═══════════════════════════════════════════════════════════════════════════════
// Test helpers
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

auto make_logger()  { return std::make_shared<TestLogger>(); }
auto make_clock()   { return std::make_shared<TestClock>(); }
auto make_metrics() { return std::make_shared<TestMetrics>(); }

/// Минимальный TradeIntent для тестов
strategy::TradeIntent make_intent(const std::string& symbol = "BTCUSDT",
                                   Side side = Side::Buy,
                                   double qty = 0.01,
                                   double mid_price = 50000.0) {
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("test-strategy");
    intent.symbol = Symbol(symbol);
    intent.side = side;
    intent.position_side = (side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
    intent.trade_side = TradeSide::Open;
    intent.signal_intent = (side == Side::Buy) ? strategy::SignalIntent::LongEntry
                                                : strategy::SignalIntent::ShortEntry;
    intent.suggested_quantity = Quantity(qty);
    intent.snapshot_mid_price = Price(mid_price);
    intent.conviction = 0.8;
    intent.generated_at = Timestamp(1000000000);
    intent.correlation_id = CorrelationId("corr-" + symbol + "-" + std::to_string(rand()));
    intent.urgency = 0.6;
    return intent;
}

/// Минимальный RiskDecision для тестов (approved)
risk::RiskDecision make_risk_approved(double qty = 0.01) {
    risk::RiskDecision rd;
    rd.verdict = risk::RiskVerdict::Approved;
    rd.approved_quantity = Quantity(qty);
    rd.allowed = true;
    rd.action = risk::RiskAction::Allow;
    return rd;
}

/// Минимальный ExecAlpha для тестов
execution_alpha::ExecutionAlphaResult make_exec_alpha(double mid_price = 50000.0) {
    execution_alpha::ExecutionAlphaResult ea;
    ea.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
    ea.should_execute = true;
    ea.urgency_score = 0.7;
    ea.recommended_limit_price = Price(mid_price);
    return ea;
}

/// Минимальный UncertaintySnapshot для тестов
uncertainty::UncertaintySnapshot make_uncertainty() {
    uncertainty::UncertaintySnapshot us;
    us.level = UncertaintyLevel::Low;
    us.aggregate_score = 0.2;
    us.size_multiplier = 1.0;
    us.execution_mode = uncertainty::ExecutionModeRecommendation::Normal;
    return us;
}

/// Минимальный close intent
strategy::TradeIntent make_close_intent(const std::string& symbol = "BTCUSDT") {
    auto intent = make_intent(symbol, Side::Sell);
    intent.trade_side = TradeSide::Close;
    intent.signal_intent = strategy::SignalIntent::LongExit;
    intent.exit_reason = strategy::ExitReason::TakeProfit;
    return intent;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// OrderFSM tests (kept from original)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("OrderFSM: Счастливый путь New → PendingAck → Open → Filled", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-1"));

    REQUIRE(fsm.current_state() == OrderState::New);
    REQUIRE_FALSE(fsm.is_terminal());

    REQUIRE(fsm.transition(OrderState::PendingAck, "Отправка"));
    REQUIRE(fsm.current_state() == OrderState::PendingAck);
    REQUIRE(fsm.is_active());

    REQUIRE(fsm.transition(OrderState::Open, "Подтверждено"));
    REQUIRE(fsm.current_state() == OrderState::Open);
    REQUIRE(fsm.is_active());

    REQUIRE(fsm.transition(OrderState::Filled, "Полное заполнение"));
    REQUIRE(fsm.current_state() == OrderState::Filled);
    REQUIRE(fsm.is_terminal());
    REQUIRE_FALSE(fsm.is_active());

    REQUIRE(fsm.history().size() == 3);
}

TEST_CASE("OrderFSM: New → PendingAck → Rejected", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-2"));

    REQUIRE(fsm.transition(OrderState::PendingAck));
    REQUIRE(fsm.transition(OrderState::Rejected, "Недостаточно средств"));
    REQUIRE(fsm.current_state() == OrderState::Rejected);
    REQUIRE(fsm.is_terminal());
}

TEST_CASE("OrderFSM: Open → CancelPending → Cancelled", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-3"));

    REQUIRE(fsm.transition(OrderState::PendingAck));
    REQUIRE(fsm.transition(OrderState::Open));
    REQUIRE(fsm.transition(OrderState::CancelPending, "Запрос отмены"));
    REQUIRE(fsm.transition(OrderState::Cancelled, "Отменён"));
    REQUIRE(fsm.is_terminal());
}

TEST_CASE("OrderFSM: Недопустимый переход → false", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-4"));
    REQUIRE_FALSE(fsm.transition(OrderState::Open));
    REQUIRE(fsm.current_state() == OrderState::New);
    REQUIRE_FALSE(fsm.transition(OrderState::Filled));
    REQUIRE(fsm.current_state() == OrderState::New);
}

TEST_CASE("OrderFSM: PartiallyFilled → Filled", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-5"));
    REQUIRE(fsm.transition(OrderState::PendingAck));
    REQUIRE(fsm.transition(OrderState::Open));
    REQUIRE(fsm.transition(OrderState::PartiallyFilled, "50% заполнено"));
    REQUIRE(fsm.transition(OrderState::Filled, "100% заполнено"));
    REQUIRE(fsm.is_terminal());
}

TEST_CASE("OrderFSM: Из терминального состояния нельзя перейти", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-6"));
    fsm.transition(OrderState::PendingAck);
    fsm.transition(OrderState::Rejected);
    REQUIRE_FALSE(fsm.transition(OrderState::Open));
    REQUIRE_FALSE(fsm.transition(OrderState::PendingAck));
    REQUIRE(fsm.current_state() == OrderState::Rejected);
}

TEST_CASE("OrderFSM: force_transition обходит валидацию", "[execution][fsm]") {
    OrderFSM fsm(OrderId("test-7"));
    // Принудительный переход из New прямо в UnknownRecovery
    fsm.force_transition(OrderState::UnknownRecovery, "Recovery");
    REQUIRE(fsm.current_state() == OrderState::UnknownRecovery);
}

// ═══════════════════════════════════════════════════════════════════════════════
// is_valid_transition tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("is_valid_transition: корректная таблица переходов", "[execution][fsm]") {
    REQUIRE(is_valid_transition(OrderState::New, OrderState::PendingAck));
    REQUIRE(is_valid_transition(OrderState::PendingAck, OrderState::Open));
    REQUIRE(is_valid_transition(OrderState::PendingAck, OrderState::Rejected));
    REQUIRE(is_valid_transition(OrderState::PendingAck, OrderState::Filled));
    REQUIRE(is_valid_transition(OrderState::Open, OrderState::PartiallyFilled));
    REQUIRE(is_valid_transition(OrderState::Open, OrderState::Filled));
    REQUIRE(is_valid_transition(OrderState::Open, OrderState::CancelPending));
    REQUIRE(is_valid_transition(OrderState::Open, OrderState::Expired));
    REQUIRE(is_valid_transition(OrderState::PartiallyFilled, OrderState::Filled));
    REQUIRE(is_valid_transition(OrderState::PartiallyFilled, OrderState::CancelPending));
    REQUIRE(is_valid_transition(OrderState::CancelPending, OrderState::Cancelled));
    REQUIRE(is_valid_transition(OrderState::CancelPending, OrderState::Filled));

    REQUIRE_FALSE(is_valid_transition(OrderState::New, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::New, OrderState::Filled));
    REQUIRE_FALSE(is_valid_transition(OrderState::Filled, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::Cancelled, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::Rejected, OrderState::PendingAck));
}

// ═══════════════════════════════════════════════════════════════════════════════
// PaperOrderSubmitter tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("PaperOrderSubmitter: Немедленное подтверждение", "[execution][paper]") {
    PaperOrderSubmitter submitter;
    OrderRecord order;
    order.order_id = OrderId("ORD-1");
    order.symbol = Symbol("BTCUSDT");
    order.side = Side::Buy;

    auto result = submitter.submit_order(order);
    REQUIRE(result.success);
    REQUIRE(result.order_id.get() == "ORD-1");
    REQUIRE_FALSE(result.exchange_order_id.get().empty());
}

TEST_CASE("PaperOrderSubmitter: Отмена всегда успешна", "[execution][paper]") {
    PaperOrderSubmitter submitter;
    REQUIRE(submitter.cancel_order(OrderId("ORD-1"), Symbol("BTCUSDT")));
}

// ═══════════════════════════════════════════════════════════════════════════════
// ClientOrderIdGenerator tests (§22)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ClientOrderIdGenerator: уникальные ID", "[execution][idempotency]") {
    auto id1 = ClientOrderIdGenerator::next();
    auto id2 = ClientOrderIdGenerator::next();
    auto id3 = ClientOrderIdGenerator::next();

    REQUIRE_FALSE(id1.empty());
    REQUIRE_FALSE(id2.empty());
    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    // Формат: TB{6digits}-{seq}
    REQUIRE(id1.substr(0, 2) == "TB");
}

// ═══════════════════════════════════════════════════════════════════════════════
// OrderRegistry tests (§6.1, §22)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("OrderRegistry: register, get, update", "[execution][registry]") {
    ExecutionConfig config;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    OrderRecord order;
    order.order_id = OrderId("R-1");
    order.symbol = Symbol("ETHUSDT");
    order.side = Side::Buy;
    registry.register_order(order);

    auto found = registry.get_order(OrderId("R-1"));
    REQUIRE(found.has_value());
    REQUIRE(found->symbol.get() == "ETHUSDT");

    auto not_found = registry.get_order(OrderId("R-999"));
    REQUIRE_FALSE(not_found.has_value());
}

TEST_CASE("OrderRegistry: FSM transitions", "[execution][registry]") {
    ExecutionConfig config;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    OrderRecord order;
    order.order_id = OrderId("FSM-1");
    registry.register_order(order);

    REQUIRE(registry.transition(OrderId("FSM-1"), OrderState::PendingAck, "test"));
    REQUIRE(registry.fsm_state(OrderId("FSM-1")) == OrderState::PendingAck);

    // Invalid transition
    REQUIRE_FALSE(registry.transition(OrderId("FSM-1"), OrderState::Expired, "test"));
    REQUIRE(registry.fsm_state(OrderId("FSM-1")) == OrderState::PendingAck);
}

TEST_CASE("OrderRegistry: active_orders и active_count", "[execution][registry]") {
    ExecutionConfig config;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    OrderRecord o1, o2;
    o1.order_id = OrderId("A-1");
    o1.state = OrderState::New;
    o2.order_id = OrderId("A-2");
    o2.state = OrderState::New;
    registry.register_order(o1);
    registry.register_order(o2);

    // New is not active (only PendingAck, Open, PartiallyFilled, CancelPending are active)
    REQUIRE(registry.active_count() == 0);

    registry.transition(OrderId("A-1"), OrderState::PendingAck);
    REQUIRE(registry.active_count() == 1);
    REQUIRE(registry.active_orders().size() == 1);
}

TEST_CASE("OrderRegistry: intent dedup (§22)", "[execution][registry][dedup]") {
    ExecutionConfig config;
    config.dedup_window_ms = 5000;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    REQUIRE_FALSE(registry.is_duplicate_intent("key1"));
    registry.record_intent("key1");
    REQUIRE(registry.is_duplicate_intent("key1"));
    REQUIRE_FALSE(registry.is_duplicate_intent("key2"));
}

TEST_CASE("OrderRegistry: fill idempotency (§22)", "[execution][registry][idempotency]") {
    ExecutionConfig config;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    REQUIRE_FALSE(registry.is_fill_applied(OrderId("F-1")));
    registry.mark_fill_applied(OrderId("F-1"));
    REQUIRE(registry.is_fill_applied(OrderId("F-1")));
    REQUIRE_FALSE(registry.is_fill_applied(OrderId("F-2")));
}

TEST_CASE("OrderRegistry: cleanup_terminal_orders", "[execution][registry]") {
    ExecutionConfig config;
    auto clk = make_clock();
    auto log = make_logger();
    OrderRegistry registry(clk, log, config);

    OrderRecord o;
    o.order_id = OrderId("T-1");
    registry.register_order(o);

    // Transition to terminal state — force_transition sets last_updated to clock_->now()
    clk->current_time = 100'000'000;
    registry.force_transition(OrderId("T-1"), OrderState::Filled, "test");

    // Not old enough yet
    clk->current_time = 100'000'000 + 1'000'000'000LL; // 1s later
    auto removed_early = registry.cleanup_terminal_orders(3'600'000'000'000LL);
    REQUIRE(removed_early == 0);

    // Now advance past max_age
    clk->current_time = 100'000'000 + 3'600'000'000'001LL; // > 1 hour later
    auto removed = registry.cleanup_terminal_orders(3'600'000'000'000LL);
    REQUIRE(removed == 1);
    REQUIRE_FALSE(registry.get_order(OrderId("T-1")).has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// ExecutionPlanner tests (§13-§14)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ExecutionPlanner: classify_action", "[execution][planner]") {
    ExecutionConfig config;
    auto log = make_logger();
    ExecutionPlanner planner(config, log);

    // Long entry
    auto intent = make_intent("BTCUSDT", Side::Buy);
    REQUIRE(planner.classify_action(intent) == ExecutionAction::OpenPosition);

    // Close
    auto close = make_close_intent();
    REQUIRE(planner.classify_action(close) == ExecutionAction::ClosePosition);

    // Emergency
    auto emergency = make_close_intent();
    emergency.exit_reason = strategy::ExitReason::EmergencyExit;
    REQUIRE(planner.classify_action(emergency) == ExecutionAction::EmergencyFlattenSymbol);

    // Reduce
    auto reduce = make_intent();
    reduce.signal_intent = strategy::SignalIntent::ReducePosition;
    REQUIRE(planner.classify_action(reduce) == ExecutionAction::ReducePosition);
}

TEST_CASE("ExecutionPlanner: plan for entry → market", "[execution][planner]") {
    ExecutionConfig config;
    auto log = make_logger();
    ExecutionPlanner planner(config, log);

    auto intent = make_intent();
    auto risk = make_risk_approved();
    auto ea = make_exec_alpha();
    MarketExecutionContext market;
    market.best_bid = Price(49990); market.best_ask = Price(50010);
    market.spread_bps = 4.0;
    auto unc = make_uncertainty();

    auto plan = planner.plan(intent, risk, ea, market, unc);

    REQUIRE(plan.action == ExecutionAction::OpenPosition);
    REQUIRE(plan.planned_quantity.get() > 0);
    REQUIRE(plan.timeout_ms > 0);
    REQUIRE_FALSE(plan.reasons.empty());
}

TEST_CASE("ExecutionPlanner: close intent → reduce_only", "[execution][planner]") {
    ExecutionConfig config;
    auto log = make_logger();
    ExecutionPlanner planner(config, log);

    auto intent = make_close_intent();
    auto risk = make_risk_approved();
    auto ea = make_exec_alpha();
    MarketExecutionContext market;
    auto unc = make_uncertainty();

    auto plan = planner.plan(intent, risk, ea, market, unc);

    REQUIRE(plan.reduce_only);
    REQUIRE(plan.action == ExecutionAction::ClosePosition);
}

TEST_CASE("ExecutionPlanner: stale data → forced market", "[execution][planner]") {
    ExecutionConfig config;
    auto log = make_logger();
    ExecutionPlanner planner(config, log);

    auto intent = make_intent();
    auto risk = make_risk_approved();
    auto ea = make_exec_alpha();
    ea.recommended_style = execution_alpha::ExecutionStyle::Passive;
    MarketExecutionContext market;
    market.is_stale = true;
    auto unc = make_uncertainty();

    auto plan = planner.plan(intent, risk, ea, market, unc);

    REQUIRE(plan.style == PlannedExecutionStyle::AggressiveMarket);
    REQUIRE(plan.order_type == OrderType::Market);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ExecutionMetrics tests (§23)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ExecutionMetrics: record & snapshot", "[execution][metrics]") {
    auto metrics = make_metrics();
    auto log = make_logger();
    ExecutionMetrics em(metrics, log);

    OrderRecord order;
    order.order_id = OrderId("M-1");
    order.symbol = Symbol("BTCUSDT");
    order.side = Side::Buy;

    em.record_submission(order);
    em.record_fill(order, Price(50100.0), 42);

    auto stats = em.snapshot();
    REQUIRE(stats.total_orders == 1);
    REQUIRE(stats.filled_orders == 1);
    REQUIRE(stats.avg_submit_to_fill_ms == 42.0);
    REQUIRE(stats.fill_rate_pct == 100.0);
}

TEST_CASE("ExecutionMetrics: rejection tracking", "[execution][metrics]") {
    auto metrics = make_metrics();
    auto log = make_logger();
    ExecutionMetrics em(metrics, log);

    OrderRecord order;
    order.order_id = OrderId("M-2");
    order.symbol = Symbol("ETHUSDT");

    em.record_submission(order);
    em.record_rejection(order, "Insufficient funds");

    auto stats = em.snapshot();
    REQUIRE(stats.total_orders == 1);
    REQUIRE(stats.rejected_orders == 1);
    REQUIRE(stats.reject_rate_pct == 100.0);
}

TEST_CASE("ExecutionMetrics: slippage tracking", "[execution][metrics]") {
    auto metrics = make_metrics();
    auto log = make_logger();
    ExecutionMetrics em(metrics, log);

    em.record_slippage(Symbol("BTCUSDT"), 50000.0, 50010.0, Side::Buy);
    auto stats = em.snapshot();
    REQUIRE(stats.avg_slippage_bps > 0);
}

TEST_CASE("ExecutionMetrics: reset", "[execution][metrics]") {
    auto metrics = make_metrics();
    auto log = make_logger();
    ExecutionMetrics em(metrics, log);

    OrderRecord order;
    order.order_id = OrderId("M-3");
    order.symbol = Symbol("BTCUSDT");
    em.record_submission(order);

    em.reset();
    auto stats = em.snapshot();
    REQUIRE(stats.total_orders == 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ExecutionEngine integration tests (§29)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ExecutionEngine: full execute → market fill", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        100000.0, log, clk, met);
    ExecutionConfig config;

    ExecutionEngine engine(submitter, portfolio, log, clk, met, config);
    engine.set_leverage(10.0);

    auto result = engine.execute(
        make_intent(), make_risk_approved(), make_exec_alpha(), make_uncertainty());

    REQUIRE(result.has_value());
    auto oid = *result;
    REQUIRE_FALSE(oid.get().empty());

    // Order should be filled (market)
    auto order = engine.get_order(oid);
    REQUIRE(order.has_value());
    REQUIRE(order->state == OrderState::Filled);
    REQUIRE(order->filled_quantity.get() > 0);
    REQUIRE(order->avg_fill_price.get() > 0);
}

TEST_CASE("ExecutionEngine: risk denied → error", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();

    ExecutionEngine engine(submitter, nullptr, log, clk, met);

    auto risk = make_risk_approved();
    risk.verdict = risk::RiskVerdict::Denied;

    auto result = engine.execute(make_intent(), risk, make_exec_alpha(), make_uncertainty());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == TbError::RiskDenied);
}

TEST_CASE("ExecutionEngine: exec alpha blocked → error", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();

    ExecutionEngine engine(submitter, nullptr, log, clk, met);

    auto ea = make_exec_alpha();
    ea.should_execute = false;

    auto result = engine.execute(make_intent(), make_risk_approved(), ea, make_uncertainty());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ExecutionEngine: extreme uncertainty → error", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();

    ExecutionEngine engine(submitter, nullptr, log, clk, met);

    auto unc = make_uncertainty();
    unc.level = UncertaintyLevel::Extreme;

    auto result = engine.execute(make_intent(), make_risk_approved(), make_exec_alpha(), unc);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ExecutionEngine: duplicate intent → rejected (§22)", "[execution][engine][dedup]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        100000.0, log, clk, met);

    ExecutionEngine engine(submitter, portfolio, log, clk, met);
    engine.set_leverage(10.0);

    auto intent = make_intent();
    // Fixed correlation_id for dedup testing
    intent.correlation_id = CorrelationId("dedup-test-1");

    auto result1 = engine.execute(intent, make_risk_approved(), make_exec_alpha(), make_uncertainty());
    REQUIRE(result1.has_value());

    // Same intent again → duplicate
    auto result2 = engine.execute(intent, make_risk_approved(), make_exec_alpha(), make_uncertainty());
    REQUIRE_FALSE(result2.has_value());
}

TEST_CASE("ExecutionEngine: zero quantity → rejected", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();

    ExecutionEngine engine(submitter, nullptr, log, clk, met);

    auto risk = make_risk_approved(0.0);  // Zero quantity!
    auto result = engine.execute(make_intent(), risk, make_exec_alpha(), make_uncertainty());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ExecutionEngine: cancel non-existent → error", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();

    ExecutionEngine engine(submitter, nullptr, log, clk, met);

    auto result = engine.cancel(OrderId("nonexistent"));
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ExecutionEngine: active_orders returns only active", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        100000.0, log, clk, met);

    ExecutionEngine engine(submitter, portfolio, log, clk, met);
    engine.set_leverage(10.0);

    // Execute market order → immediately filled → terminal
    (void)engine.execute(make_intent(), make_risk_approved(), make_exec_alpha(), make_uncertainty());

    // Market orders go directly to Filled → not active
    auto active = engine.active_orders();
    REQUIRE(active.empty());
}

TEST_CASE("ExecutionEngine: cleanup_terminal_orders", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        100000.0, log, clk, met);

    ExecutionEngine engine(submitter, portfolio, log, clk, met);
    engine.set_leverage(10.0);

    auto result = engine.execute(make_intent(), make_risk_approved(),
                                  make_exec_alpha(), make_uncertainty());
    REQUIRE(result.has_value());

    // Advance clock far enough
    clk->current_time += 3600'000'000'001LL;
    auto removed = engine.cleanup_terminal_orders(3600'000'000'000LL);
    REQUIRE(removed == 1);
}

TEST_CASE("ExecutionEngine: execution_stats reflects submissions", "[execution][engine][metrics]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto log = make_logger();
    auto clk = make_clock();
    auto met = make_metrics();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        100000.0, log, clk, met);

    ExecutionEngine engine(submitter, portfolio, log, clk, met);
    engine.set_leverage(10.0);

    (void)engine.execute(make_intent(), make_risk_approved(), make_exec_alpha(), make_uncertainty());

    auto stats = engine.execution_stats();
    REQUIRE(stats.total_orders >= 1);
    REQUIRE(stats.filled_orders >= 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ExecutionTypes tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ExecutionPlan: summary formats correctly", "[execution][types]") {
    ExecutionPlan plan;
    plan.action = ExecutionAction::OpenPosition;
    plan.style = PlannedExecutionStyle::AggressiveMarket;
    plan.order_type = OrderType::Market;
    plan.planned_quantity = Quantity(0.5);
    plan.planned_price = Price(50000.0);
    plan.timeout_ms = 15000;

    auto s = plan.summary();
    REQUIRE(s.find("OpenPosition") != std::string::npos);
    REQUIRE(s.find("market") != std::string::npos);
}

TEST_CASE("to_string functions cover all enums", "[execution][types]") {
    REQUIRE(to_string(ExecutionAction::OpenPosition) == "OpenPosition");
    REQUIRE(to_string(ExecutionAction::EmergencyFlattenAll) == "EmergencyFlattenAll");
    REQUIRE(to_string(PlannedExecutionStyle::AggressiveMarket) == "AggressiveMarket");
    REQUIRE(to_string(PlannedExecutionStyle::ReduceOnly) == "ReduceOnly");
    REQUIRE(to_string(IntentState::Received) == "Received");
    REQUIRE(to_string(IntentState::CompletedWithWarnings) == "CompletedWithWarnings");
    REQUIRE(to_string(ErrorClass::Transient) == "Transient");
    REQUIRE(to_string(ErrorClass::Critical) == "Critical");
}

TEST_CASE("IntentExecution: latency_ms calculation", "[execution][types]") {
    IntentExecution ie;
    ie.received_at_ns = 1'000'000'000;
    ie.completed_at_ns = 1'042'000'000;
    REQUIRE(ie.latency_ms() == 42);

    // Zero values
    IntentExecution ie2;
    REQUIRE(ie2.latency_ms() == 0);
}
