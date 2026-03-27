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

// ============================================================
// Тесты новых диагностических типов (Phase 3)
// ============================================================

TEST_CASE("Diag: Severity и CorrectiveAction автоматически вычисляются", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    SECTION("TradeTaken → Info, Observe") {
        auto record = engine.explain_trade(
            make_approved_decision(), make_approved_risk(),
            "Stable", "Trending", "Low");

        CHECK(record.severity == DiagnosticSeverity::Info);
        CHECK(record.recommended_action == CorrectiveAction::Observe);
    }

    SECTION("TradeDenied → severity зависит от факторов") {
        auto record = engine.explain_denial(
            make_denied_decision(), make_denied_risk(),
            "Disrupted", "Volatile", "Extreme");

        // Denied + kill_switch → серьёзные негативные факторы
        CHECK(record.severity >= DiagnosticSeverity::Warning);
        CHECK(record.recommended_action == CorrectiveAction::Observe);
    }

    SECTION("DegradedState с kill switch → Critical/Fatal, ReduceSize+") {
        auto record = engine.diagnose_system_state(
            "Disrupted", "Volatile", "Extreme", 90.0, 7.0, true);

        CHECK(record.type == DiagnosticType::DegradedState);
        CHECK(record.severity >= DiagnosticSeverity::Critical);
        CHECK(record.recommended_action != CorrectiveAction::Observe);
    }
}

TEST_CASE("Diag: Ошибка исполнения ордера", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    SECTION("Единичная ошибка") {
        auto record = engine.diagnose_execution_failure(
            Symbol("BTCUSDT"), StrategyId("momentum"),
            CorrelationId("corr-456"), "Insufficient balance", 1);

        CHECK(record.type == DiagnosticType::ExecutionFailure);
        CHECK(record.symbol.get() == "BTCUSDT");
        CHECK(record.strategy_id == "momentum");
        CHECK(record.recommended_action == CorrectiveAction::SlowDown);
        CHECK(record.factors.size() >= 2);
    }

    SECTION("Множественные ошибки → эскалация") {
        auto record = engine.diagnose_execution_failure(
            Symbol("ETHUSDT"), StrategyId("breakout"),
            CorrelationId("corr-789"), "Rate limited", 5);

        CHECK(record.type == DiagnosticType::ExecutionFailure);
        CHECK(record.severity >= DiagnosticSeverity::Critical);
        CHECK(record.recommended_action >= CorrectiveAction::StopEntries);
    }
}

TEST_CASE("Diag: Деградация рыночных данных", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_market_data_degradation(
        Symbol("BTCUSDT"), "stale_orderbook",
        15'000'000'000LL,  // 15 секунд
        150.0);             // 150 bps спред

    CHECK(record.type == DiagnosticType::MarketDataDegradation);
    CHECK(record.symbol.get() == "BTCUSDT");
    CHECK(record.severity >= DiagnosticSeverity::Critical);
    CHECK(record.recommended_action == CorrectiveAction::HaltSymbol);
    CHECK(record.factors.size() >= 3);
}

TEST_CASE("Diag: Инцидент подключения к бирже", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_connectivity_incident(
        "wss://ws.bitget.com/v2/ws/public",
        "Connection reset by peer", 3);

    CHECK(record.type == DiagnosticType::ExchangeConnectivityIncident);
    CHECK(record.factors.size() >= 3);
    CHECK(record.recommended_action != CorrectiveAction::Observe);
}

TEST_CASE("Diag: Подавление стратегии", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_strategy_suppression(
        StrategyId("mean_reversion"),
        "alpha_decay", "Деградация альфа ниже порога",
        0.15);

    CHECK(record.type == DiagnosticType::StrategySuppression);
    CHECK(record.strategy_id == "mean_reversion");
    CHECK(record.severity >= DiagnosticSeverity::Warning);
    CHECK(record.recommended_action == CorrectiveAction::Observe);
}

TEST_CASE("Diag: Ограничение портфеля", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_portfolio_constraint(
        Symbol("BTCUSDT"), "max_concentration",
        85.0, 80.0);  // 85% при лимите 80%

    CHECK(record.type == DiagnosticType::PortfolioConstraint);
    CHECK(record.symbol.get() == "BTCUSDT");
    CHECK(record.recommended_action == CorrectiveAction::ReduceSize);
}

TEST_CASE("Diag: Действие восстановления", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    SECTION("Успешное восстановление") {
        auto record = engine.diagnose_recovery_action(
            "Переподключение к WebSocket",
            "Потеря связи с биржей", true);

        CHECK(record.type == DiagnosticType::RecoveryAction);
        CHECK(record.verdict.find("выполнено") != std::string::npos);
        CHECK(record.recommended_action == CorrectiveAction::Observe);
    }

    SECTION("Неудачное восстановление") {
        auto record = engine.diagnose_recovery_action(
            "Сверка позиций", "Расхождение балансов", false);

        CHECK(record.type == DiagnosticType::RecoveryAction);
        CHECK(record.verdict.find("не удалось") != std::string::npos);
    }
}

TEST_CASE("Diag: Расхождение при сверке с биржей", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_reconciliation_mismatch(
        Symbol("BTCUSDT"), "position_quantity",
        "0.05 BTC", "0.03 BTC");

    CHECK(record.type == DiagnosticType::ReconciliationMismatch);
    CHECK(record.severity >= DiagnosticSeverity::Critical);
    CHECK(record.recommended_action == CorrectiveAction::ForceReconcile);
    CHECK(record.factors.size() >= 3);
}

// ============================================================
// Тесты Scorecards и агрегации
// ============================================================

TEST_CASE("Diag: Scorecards агрегируют статистику", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    // Генерируем записи
    engine.explain_trade(make_approved_decision(), make_approved_risk(),
        "Stable", "Trending", "Low");
    engine.explain_trade(make_approved_decision(), make_approved_risk(),
        "Stable", "Trending", "Low");
    engine.explain_denial(make_denied_decision(), make_denied_risk(),
        "Disrupted", "Volatile", "Extreme");
    engine.diagnose_execution_failure(
        Symbol("BTCUSDT"), StrategyId("momentum"),
        CorrelationId("corr-x"), "Error", 1);

    auto summary = engine.get_summary();
    CHECK(summary.total_records == 4);
    CHECK(summary.type_counts["TradeTaken"] == 2);
    CHECK(summary.type_counts["TradeDenied"] == 1);
    CHECK(summary.type_counts["ExecutionFailure"] == 1);

    // Scorecard по стратегии momentum
    auto sc = engine.get_strategy_scorecard("momentum");
    CHECK(sc.strategy_id == "momentum");
    CHECK(sc.trades_taken == 2);
    CHECK(sc.trades_denied >= 1);
    CHECK(sc.execution_failures == 1);
    CHECK(sc.denial_rate > 0.0);
    CHECK(sc.denial_rate < 1.0);
}

TEST_CASE("Diag: Буфер последних записей", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    for (int i = 0; i < 5; ++i) {
        engine.diagnose_system_state(
            "Stable", "Trending", "Low", 30.0, 1.0, false);
    }

    auto recent = engine.get_recent_records(3);
    CHECK(recent.size() == 3);

    auto all = engine.get_recent_records(100);
    CHECK(all.size() == 5);
}

TEST_CASE("Diag: Сброс scorecards", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    engine.explain_trade(make_approved_decision(), make_approved_risk(),
        "Stable", "Trending", "Low");

    auto before = engine.get_summary();
    CHECK(before.total_records == 1);

    engine.reset_scorecards();

    auto after = engine.get_summary();
    CHECK(after.total_records == 0);
    CHECK(engine.get_recent_records(100).empty());
}

TEST_CASE("Diag: JSON содержит severity и recommended_action", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto record = engine.diagnose_execution_failure(
        Symbol("BTCUSDT"), StrategyId("momentum"),
        CorrelationId("corr-json"), "Test error", 3);

    std::string json = record.machine_json;
    CHECK(json.find("\"severity\"") != std::string::npos);
    CHECK(json.find("\"recommended_action\"") != std::string::npos);
    CHECK(json.find("\"created_at\"") != std::string::npos);
}

TEST_CASE("Diag: Human summary для новых типов", "[self_diagnosis]") {
    SelfDiagnosisEngine engine;

    auto exec_record = engine.diagnose_execution_failure(
        Symbol("BTCUSDT"), StrategyId("momentum"),
        CorrelationId("corr-hs"), "Timeout", 2);
    CHECK(exec_record.human_summary.find("Ошибка исполнения") != std::string::npos);

    auto recon_record = engine.diagnose_reconciliation_mismatch(
        Symbol("ETHUSDT"), "balance", "100", "95");
    CHECK(recon_record.human_summary.find("Расхождение") != std::string::npos);

    auto recov_record = engine.diagnose_recovery_action(
        "Перезапуск", "Сбой", true);
    CHECK(recov_record.human_summary.find("Восстановление") != std::string::npos);
}
