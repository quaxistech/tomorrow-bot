/**
 * @file test_self_diagnosis.cpp
 * @brief Тесты самодиагностики системы
 */
#include <catch2/catch_test_macros.hpp>
#include "self_diagnosis/self_diagnosis_engine.hpp"

using namespace tb;
using namespace tb::self_diagnosis;

/// Создать DecisionRecord для тестов (одобренная сделка)
static decision::DecisionRecord make_approved_decision() {
    decision::DecisionRecord dr;
    dr.correlation_id = CorrelationId("corr-123");
    dr.symbol = Symbol("BTCUSDT");
    dr.decided_at = Timestamp(1000000);
    dr.trade_approved = true;
    dr.final_conviction = 0.85;
    dr.regime = RegimeLabel::Trending;
    dr.world_state = WorldStateLabel::Stable;
    dr.uncertainty = UncertaintyLevel::Low;
    dr.uncertainty_score = 0.2;

    // Вклад стратегии
    strategy::TradeIntent intent;
    intent.strategy_id = StrategyId("momentum");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.conviction = 0.9;
    intent.signal_name = "ma_crossover";

    decision::StrategyContribution contrib;
    contrib.strategy_id = StrategyId("momentum");
    contrib.intent = intent;
    contrib.weight = 0.7;
    contrib.was_vetoed = false;

    dr.contributions.push_back(contrib);

    // Финальный intent
    dr.final_intent = intent;
    dr.rationale = "MA crossover signal in trending regime";

    return dr;
}

/// Создать DecisionRecord для тестов (отклонённая сделка)
static decision::DecisionRecord make_denied_decision() {
    auto dr = make_approved_decision();
    dr.trade_approved = false;
    dr.final_conviction = 0.3;

    // Глобальное вето
    decision::VetoReason veto;
    veto.source = "uncertainty";
    veto.reason = "Экстремальная неопределённость";
    veto.severity = 0.9;
    dr.global_vetoes.push_back(veto);

    // Вето на вкладе стратегии
    dr.contributions[0].was_vetoed = true;
    decision::VetoReason strat_veto;
    strat_veto.source = "regime";
    strat_veto.reason = "Режим не подходит";
    strat_veto.severity = 0.7;
    dr.contributions[0].veto_reasons.push_back(strat_veto);

    return dr;
}

/// Создать RiskDecision (одобренная)
static risk::RiskDecision make_approved_risk() {
    risk::RiskDecision rd;
    rd.verdict = risk::RiskVerdict::Approved;
    rd.approved_quantity = Quantity(0.1);
    rd.risk_utilization_pct = 0.3;
    rd.kill_switch_active = false;
    rd.decided_at = Timestamp(1000000);
    rd.summary = "Одобрено без ограничений";
    return rd;
}

/// Создать RiskDecision (отклонённая)
static risk::RiskDecision make_denied_risk() {
    risk::RiskDecision rd;
    rd.verdict = risk::RiskVerdict::Denied;
    rd.approved_quantity = Quantity(0.0);
    rd.risk_utilization_pct = 0.95;
    rd.kill_switch_active = true;
    rd.decided_at = Timestamp(1000000);
    rd.summary = "Kill switch активен";

    risk::RiskReasonCode reason;
    reason.code = "KILL_SWITCH";
    reason.message = "Аварийный выключатель активен";
    reason.severity = 1.0;
    rd.reasons.push_back(reason);

    return rd;
}

TEST_CASE("Diag: Объяснение совершённой сделки", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.explain_trade(
        make_approved_decision(),
        make_approved_risk(),
        "Stable", "Trending", "Low");

    CHECK(record.type == DiagnosticType::TradeTaken);
    CHECK(record.trade_executed);
    CHECK(record.correlation_id.get() == "corr-123");
    CHECK(record.symbol.get() == "BTCUSDT");
    CHECK(record.world_state == "Stable");
    CHECK(record.regime == "Trending");
    CHECK(record.uncertainty_level == "Low");

    // Должны быть факторы
    REQUIRE(record.factors.size() >= 3);

    // Проверяем наличие стратегического фактора
    bool has_strategy_factor = false;
    for (const auto& f : record.factors) {
        if (f.component.starts_with("strategy:")) {
            has_strategy_factor = true;
            break;
        }
    }
    CHECK(has_strategy_factor);

    CHECK(record.diagnostic_id > 0);
}

TEST_CASE("Diag: Объяснение отказа от сделки", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.explain_denial(
        make_denied_decision(),
        make_denied_risk(),
        "Disrupted", "Volatile", "Extreme");

    CHECK(record.type == DiagnosticType::TradeDenied);
    CHECK_FALSE(record.trade_executed);
    CHECK(record.world_state == "Disrupted");

    // Должны быть факторы отклонения (вето + риск)
    REQUIRE(record.factors.size() >= 3);

    // Проверяем наличие вето-фактора
    bool has_veto = false;
    bool has_risk_denial = false;
    for (const auto& f : record.factors) {
        if (f.component.starts_with("veto:")) has_veto = true;
        if (f.component.starts_with("risk:")) has_risk_denial = true;
    }
    CHECK(has_veto);
    CHECK(has_risk_denial);
}

TEST_CASE("Diag: Диагностика состояния системы", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    SECTION("Нормальное состояние") {
        auto record = engine.diagnose_system_state(
            "Stable", "Trending", "Low", 30.0, 1.0, false);

        CHECK(record.type == DiagnosticType::SystemState);
        CHECK(record.verdict == "Система работает нормально");
        REQUIRE(record.factors.size() >= 5);
    }

    SECTION("Деградированное состояние — kill switch") {
        auto record = engine.diagnose_system_state(
            "Stable", "Trending", "Low", 30.0, 1.0, true);

        CHECK(record.type == DiagnosticType::DegradedState);
        CHECK(record.verdict == "Система в деградированном состоянии");

        // Проверяем фактор kill switch
        bool has_kill = false;
        for (const auto& f : record.factors) {
            if (f.observation.find("KILL SWITCH") != std::string::npos) {
                has_kill = true;
                CHECK(f.impact == -1.0);
            }
        }
        CHECK(has_kill);
    }

    SECTION("Деградированное состояние — высокая просадка") {
        auto record = engine.diagnose_system_state(
            "Stable", "Trending", "Low", 30.0, 6.0, false);

        CHECK(record.type == DiagnosticType::DegradedState);
    }

    SECTION("Деградированное состояние — disrupted world") {
        auto record = engine.diagnose_system_state(
            "Disrupted", "Volatile", "Extreme", 80.0, 3.0, false);

        CHECK(record.type == DiagnosticType::DegradedState);
    }
}

TEST_CASE("Diag: Генерация человекочитаемого резюме", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.explain_trade(
        make_approved_decision(),
        make_approved_risk(),
        "Stable", "Trending", "Low");

    std::string summary = SelfDiagnosisEngine::generate_human_summary(record);
    CHECK_FALSE(summary.empty());
    CHECK(summary.find("Сделка совершена") != std::string::npos);
}

TEST_CASE("Diag: Генерация машиночитаемого JSON", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.explain_trade(
        make_approved_decision(),
        make_approved_risk(),
        "Stable", "Trending", "Low");

    std::string json = SelfDiagnosisEngine::generate_machine_json(record);
    CHECK_FALSE(json.empty());
    CHECK(json.front() == '{');
    CHECK(json.back() == '}');
    CHECK(json.find("\"diagnostic_id\"") != std::string::npos);
    CHECK(json.find("\"type\"") != std::string::npos);
    CHECK(json.find("\"factors\"") != std::string::npos);
    CHECK(json.find("TradeTaken") != std::string::npos);
}

TEST_CASE("Diag: ID диагностики уникальны", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto r1 = engine.explain_trade(
        make_approved_decision(), make_approved_risk(),
        "Stable", "Trending", "Low");

    auto r2 = engine.explain_denial(
        make_denied_decision(), make_denied_risk(),
        "Disrupted", "Volatile", "Extreme");

    auto r3 = engine.diagnose_system_state(
        "Stable", "Trending", "Low", 30.0, 1.0, false);

    CHECK(r1.diagnostic_id != r2.diagnostic_id);
    CHECK(r2.diagnostic_id != r3.diagnostic_id);
    CHECK(r1.diagnostic_id != r3.diagnostic_id);
}
