#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "execution/execution_engine.hpp"
#include "execution/order_fsm.hpp"
#include "execution/order_types.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

using namespace tb;
using namespace tb::execution;
using namespace Catch::Matchers;

// ========== Тестовые заглушки ==========

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

class TestMetrics : public metrics::IMetricsRegistry {
    struct NullCounter : metrics::ICounter {
        std::string name_{"null"};
        void increment(double /*v*/) override {}
        void increment(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullGauge : metrics::IGauge {
        std::string name_{"null"};
        void set(double /*v*/) override {}
        void set(double /*v*/, const metrics::MetricTags&) override {}
        void increment(double /*v*/) override {}
        void decrement(double /*v*/) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullHistogram : metrics::IHistogram {
        std::string name_{"null"};
        void observe(double /*v*/) override {}
        void observe(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override {
        return std::make_shared<NullCounter>();
    }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override {
        return std::make_shared<NullGauge>();
    }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override {
        return std::make_shared<NullHistogram>();
    }
    [[nodiscard]] std::string export_prometheus() const override { return ""; }
};

// ========== Тесты OrderFSM ==========

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

    // New → Open (должен быть через PendingAck)
    REQUIRE_FALSE(fsm.transition(OrderState::Open));
    REQUIRE(fsm.current_state() == OrderState::New);

    // New → Filled (недопустимо)
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

    // Из Rejected никуда
    REQUIRE_FALSE(fsm.transition(OrderState::Open));
    REQUIRE_FALSE(fsm.transition(OrderState::PendingAck));
    REQUIRE(fsm.current_state() == OrderState::Rejected);
}

// ========== Тесты is_valid_transition ==========

TEST_CASE("is_valid_transition: корректная таблица переходов", "[execution][fsm]") {
    // Допустимые переходы
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
    REQUIRE(is_valid_transition(OrderState::CancelPending, OrderState::PartiallyFilled));

    // Недопустимые переходы
    REQUIRE_FALSE(is_valid_transition(OrderState::New, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::New, OrderState::Filled));
    REQUIRE_FALSE(is_valid_transition(OrderState::Filled, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::Cancelled, OrderState::Open));
    REQUIRE_FALSE(is_valid_transition(OrderState::Rejected, OrderState::PendingAck));

    // UnknownRecovery — любой нетерминальный может перейти
    REQUIRE(is_valid_transition(OrderState::New, OrderState::PendingAck));
    REQUIRE(is_valid_transition(OrderState::Open, OrderState::UnknownRecovery));
    REQUIRE(is_valid_transition(OrderState::PendingAck, OrderState::UnknownRecovery));
}

// ========== Тесты PaperOrderSubmitter ==========

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
    REQUIRE(submitter.cancel_order(OrderId("ORD-1")));
}

// ========== Тесты ExecutionEngine ==========

TEST_CASE("ExecutionEngine: Создание ордера корректно", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        10000.0,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    ExecutionEngine engine(
        submitter, portfolio,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("test");
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.suggested_quantity = Quantity(0.1);
    intent.limit_price = Price(50000.0);
    intent.conviction = 0.7;
    intent.correlation_id = CorrelationId("corr-1");

    risk::RiskDecision risk_decision;
    risk_decision.verdict = risk::RiskVerdict::Approved;
    risk_decision.approved_quantity = Quantity(0.1);

    execution_alpha::ExecutionAlphaResult exec_alpha;
    exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Passive;
    exec_alpha.recommended_limit_price = Price(49990.0);

    auto result = engine.execute(intent, risk_decision, exec_alpha);

    REQUIRE(result.has_value());
    auto order = engine.get_order(*result);
    REQUIRE(order.has_value());
    REQUIRE(order->symbol.get() == "BTCUSDT");
    REQUIRE(order->side == Side::Buy);
    REQUIRE(order->state == OrderState::PendingAck);
}

TEST_CASE("ExecutionEngine: Risk denied → ошибка", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        10000.0,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    ExecutionEngine engine(
        submitter, portfolio,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("test");
    intent.symbol = Symbol("BTCUSDT");
    intent.correlation_id = CorrelationId("corr-2");

    risk::RiskDecision risk_decision;
    risk_decision.verdict = risk::RiskVerdict::Denied;

    execution_alpha::ExecutionAlphaResult exec_alpha;

    auto result = engine.execute(intent, risk_decision, exec_alpha);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ExecutionEngine: Обнаружение дубликатов", "[execution][engine]") {
    auto submitter = std::make_shared<PaperOrderSubmitter>();
    auto portfolio = std::make_shared<portfolio::InMemoryPortfolioEngine>(
        10000.0,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    ExecutionEngine engine(
        submitter, portfolio,
        std::make_shared<TestLogger>(),
        std::make_shared<TestClock>(),
        std::make_shared<TestMetrics>());

    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("test");
    intent.symbol = Symbol("BTCUSDT");
    intent.correlation_id = CorrelationId("corr-dup");

    risk::RiskDecision risk_decision;
    risk_decision.verdict = risk::RiskVerdict::Approved;
    risk_decision.approved_quantity = Quantity(0.1);

    execution_alpha::ExecutionAlphaResult exec_alpha;
    exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Passive;

    // Первый ордер — успех
    auto result1 = engine.execute(intent, risk_decision, exec_alpha);
    REQUIRE(result1.has_value());

    // Второй с тем же correlation_id + symbol — дубликат
    auto result2 = engine.execute(intent, risk_decision, exec_alpha);
    REQUIRE_FALSE(result2.has_value());
}

TEST_CASE("ExecutionEngine: to_string для OrderState", "[execution]") {
    REQUIRE(to_string(OrderState::New) == "New");
    REQUIRE(to_string(OrderState::PendingAck) == "PendingAck");
    REQUIRE(to_string(OrderState::Open) == "Open");
    REQUIRE(to_string(OrderState::PartiallyFilled) == "PartiallyFilled");
    REQUIRE(to_string(OrderState::Filled) == "Filled");
    REQUIRE(to_string(OrderState::CancelPending) == "CancelPending");
    REQUIRE(to_string(OrderState::Cancelled) == "Cancelled");
    REQUIRE(to_string(OrderState::Rejected) == "Rejected");
    REQUIRE(to_string(OrderState::Expired) == "Expired");
    REQUIRE(to_string(OrderState::UnknownRecovery) == "UnknownRecovery");
}
