/**
 * @file protective_bracket_manager_test.cpp
 * @brief Тесты для ProtectiveBracketManager (edge-31 Phase 3).
 *
 * Тестируем lifecycle bracket:
 *   - on_position_opened регистрирует состояние
 *   - verify_brackets с grace period и max_attempts
 *   - update_sl: cancel+replace
 *   - release: cancel both legs
 *   - recover_from_exchange: pickup существующих brackets с биржи
 *
 * Bitget API мокается через подменяемые submitter+query stubs.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pipeline/protective_bracket_manager.hpp"
#include "test_mocks.hpp"

#include <memory>
#include <vector>

using namespace tb;
using namespace tb::pipeline;
using namespace tb::test;
using namespace tb::exchange::bitget;

namespace {

/// Stub submitter — counts submit/cancel calls and returns canned responses.
class StubSubmitter : public BitgetFuturesOrderSubmitter {
public:
    StubSubmitter()
        : BitgetFuturesOrderSubmitter(
              nullptr,  // rest_client
              std::make_shared<TestLogger>(),
              config::FuturesConfig{}) {}

    int submit_calls{0};
    int cancel_calls{0};
    bool submit_success{true};
    std::string next_exchange_id{"EXCH-1"};
    std::vector<std::pair<std::string, double>> submitted_triggers; // (exchange_id, trigger)

    execution::OrderSubmitResult submit_plan_order(
        const execution::OrderRecord& order) override
    {
        ++submit_calls;
        execution::OrderSubmitResult r;
        r.order_id = order.order_id;
        r.success = submit_success;
        if (submit_success) {
            r.exchange_order_id = OrderId(next_exchange_id);
            submitted_triggers.emplace_back(next_exchange_id, order.plan_params.trigger_price.get());
        } else {
            r.error_message = "stub_fail";
        }
        return r;
    }

    std::vector<std::string> cancel_plan_types;
    bool cancel_plan_order(const OrderId& /*id*/, const Symbol& /*sym*/,
                            const std::string& plan_type) override {
        ++cancel_calls;
        cancel_plan_types.push_back(plan_type);
        return true;
    }
};

/// Stub query adapter — returns canned plan-orders.
class StubQuery : public BitgetFuturesQueryAdapter {
public:
    StubQuery()
        : BitgetFuturesQueryAdapter(
              nullptr,
              std::make_shared<TestLogger>(),
              config::FuturesConfig{}) {}

    std::vector<PlanOrderInfo> plans;
    int query_calls{0};
    bool query_succeed{true};

    Result<std::vector<PlanOrderInfo>> get_open_plan_orders(const Symbol& /*sym*/) override {
        ++query_calls;
        if (!query_succeed) {
            return Err<std::vector<PlanOrderInfo>>(TbError::ExchangeConnectionFailed);
        }
        return plans;
    }
};

PlanOrderInfo make_plan(const std::string& id,
                         PlanOrderKind kind,
                         double trigger,
                         PositionSide ps = PositionSide::Long,
                         const std::string& sym = "BTCUSDT") {
    PlanOrderInfo p;
    p.order_id = OrderId(id);
    p.symbol = Symbol(sym);
    p.position_side = ps;
    p.kind = kind;
    p.trigger_price = Price(trigger);
    return p;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ProtectiveBracketManager: on_position_opened регистрирует state",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketManager mgr(sub, qry, log, clk);
    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(st.has_value());
    REQUIRE(st->tp_price == 50750.0);
    REQUIRE(st->sl_price == 49500.0);
    REQUIRE(st->position_size == 0.01);
    REQUIRE(st->entry_price == 50000.0);
    REQUIRE_FALSE(st->verified);
    REQUIRE(st->tp_source == BracketSource::PresetAttached);
    REQUIRE(st->sl_source == BracketSource::PresetAttached);
}

TEST_CASE("ProtectiveBracketManager: verify в grace period возвращает false",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 1500;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    // ещё в grace period (500ms прошло)
    clk->current_time += 500'000'000LL;
    REQUIRE_FALSE(mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long));
    REQUIRE(qry->query_calls == 0);  // не должен звонить API
}

TEST_CASE("ProtectiveBracketManager: verify находит preset SL+TP → verified=true",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 1000;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    qry->plans = {
        make_plan("PL-TP", PlanOrderKind::ProfitPlan, 50750.0),
        make_plan("PL-SL", PlanOrderKind::LossPlan, 49500.0),
    };

    clk->current_time += 2'000'000'000LL;  // прошёл grace
    REQUIRE(mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long));

    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(st->verified);
    REQUIRE(st->tp_order_id.get() == "PL-TP");
    REQUIRE(st->sl_order_id.get() == "PL-SL");
}

TEST_CASE("ProtectiveBracketManager: fallback после max_attempts ставит standalone",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 100;
    cfg.max_verify_attempts = 2;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    qry->plans = {};  // на бирже ничего не нашлось — нужен fallback
    sub->next_exchange_id = "STUB-FALLBACK-SL";

    clk->current_time += 200'000'000LL;  // > grace
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);  // attempt 1, не fallback
    clk->current_time += 1'200'000'000LL;
    sub->next_exchange_id = "STUB-FALLBACK-TP";
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);  // attempt 2, fallback

    REQUIRE(sub->submit_calls == 2);  // SL и TP fallback
    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(st->verified);
    REQUIRE(st->sl_source == BracketSource::StandalonePlan);
    REQUIRE(st->tp_source == BracketSource::StandalonePlan);
}

TEST_CASE("ProtectiveBracketManager: update_sl no-op (TP/SL ставятся один раз)",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 100;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    qry->plans = {
        make_plan("PL-SL", PlanOrderKind::LossPlan, 49500.0),
        make_plan("PL-TP", PlanOrderKind::ProfitPlan, 50750.0),
    };
    clk->current_time += 200'000'000LL;
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);

    REQUIRE_FALSE(mgr.update_sl(Symbol("BTCUSDT"), PositionSide::Long, 49800.0));
    REQUIRE(sub->submit_calls == 0);
    REQUIRE(sub->cancel_calls == 0);

    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(st->sl_price == 49500.0);
}

TEST_CASE("ProtectiveBracketManager: release отменяет оба плеча",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 100;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    qry->plans = {
        make_plan("PL-TP", PlanOrderKind::ProfitPlan, 50750.0),
        make_plan("PL-SL", PlanOrderKind::LossPlan, 49500.0),
    };
    clk->current_time += 200'000'000LL;
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);

    REQUIRE(mgr.release(Symbol("BTCUSDT"), PositionSide::Long));
    REQUIRE(sub->cancel_calls == 2);  // TP + SL отменены
    REQUIRE_FALSE(mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long).has_value());
}

TEST_CASE("ProtectiveBracketManager: recover_from_exchange — pickup на старте",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    qry->plans = {
        make_plan("PL-TP", PlanOrderKind::ProfitPlan, 50800.0),
        make_plan("PL-SL", PlanOrderKind::LossPlan, 49200.0),
    };

    ProtectiveBracketManager mgr(sub, qry, log, clk);
    int n = mgr.recover_from_exchange({{Symbol("BTCUSDT"), PositionSide::Long}});
    REQUIRE(n == 1);

    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Long);
    REQUIRE(st.has_value());
    REQUIRE(st->verified);
    REQUIRE(st->tp_order_id.get() == "PL-TP");
    REQUIRE(st->sl_order_id.get() == "PL-SL");
    REQUIRE(st->tp_price == 50800.0);
    REQUIRE(st->sl_price == 49200.0);
}

TEST_CASE("ProtectiveBracketManager: SHORT position верифицируется отдельно",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 100;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    // SHORT: SL выше mid, TP ниже mid
    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Short,
                            50000.0, 0.01, 49250.0, 50500.0);

    qry->plans = {
        make_plan("PL-TP-SHORT", PlanOrderKind::ProfitPlan, 49250.0, PositionSide::Short),
        make_plan("PL-SL-SHORT", PlanOrderKind::LossPlan, 50500.0, PositionSide::Short),
        // Шумовая запись — другой position_side, не должна подцепиться
        make_plan("PL-TP-LONG", PlanOrderKind::ProfitPlan, 49250.0, PositionSide::Long),
    };

    clk->current_time += 200'000'000LL;
    REQUIRE(mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Short));

    auto st = mgr.get_state(Symbol("BTCUSDT"), PositionSide::Short);
    REQUIRE(st->verified);
    REQUIRE(st->tp_order_id.get() == "PL-TP-SHORT");
    REQUIRE(st->sl_order_id.get() == "PL-SL-SHORT");
}

TEST_CASE("ProtectiveBracketManager: query failure → retry не считается attempt fail",
          "[bracket][edge-31]") {
    auto sub = std::make_shared<StubSubmitter>();
    auto qry = std::make_shared<StubQuery>();
    auto log = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    clk->current_time = 10'000'000'000LL;

    ProtectiveBracketConfig cfg;
    cfg.verify_grace_ms = 100;
    cfg.max_verify_attempts = 2;
    ProtectiveBracketManager mgr(sub, qry, log, clk, cfg);

    mgr.on_position_opened(Symbol("BTCUSDT"), PositionSide::Long,
                            50000.0, 0.01, 50750.0, 49500.0);

    qry->query_succeed = false;
    clk->current_time += 200'000'000LL;
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);
    (void)mgr.verify_brackets(Symbol("BTCUSDT"), PositionSide::Long);

    // submit_calls должен быть 0 — fallback ставится только когда query SUCCESS
    // вернул пустой список. При query failure мы просто увеличиваем attempts.
    REQUIRE(sub->submit_calls == 0);
}
