/**
 * @file self_diagnosis_engine.cpp
 * @brief Реализация движка самодиагностики
 *
 * Формирует объяснения решений: почему сделка совершена или отклонена,
 * диагностирует состояние системы и генерирует человеко- и машиночитаемые отчёты.
 */
#include "self_diagnosis/self_diagnosis_engine.hpp"
#include <sstream>

namespace tb::self_diagnosis {

SelfDiagnosisEngine::SelfDiagnosisEngine() = default;

uint64_t SelfDiagnosisEngine::next_diagnostic_id() {
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================
// Объяснение совершённой сделки
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::explain_trade(
    const decision::DecisionRecord& decision,
    const risk::RiskDecision& risk_decision,
    const std::string& world_state,
    const std::string& regime,
    const std::string& uncertainty_level)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::TradeTaken;
    record.correlation_id = decision.correlation_id;
    record.symbol = decision.symbol;
    record.created_at = decision.decided_at;
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;
    record.trade_executed = true;

    // Стратегия и риск-вердикт
    if (decision.final_intent.has_value()) {
        record.strategy_id = decision.final_intent->strategy_id.get();
    }
    record.risk_verdict = risk_decision.summary;

    // Извлекаем факторы из решения и риска
    extract_decision_factors(decision, record.factors);
    extract_risk_factors(risk_decision, record.factors);

    // Контекстные факторы
    record.factors.push_back({"world_model", "Состояние мира: " + world_state,
        world_state == "Stable" ? 0.3 : (world_state == "Disrupted" ? -0.8 : 0.0)});
    record.factors.push_back({"regime", "Режим рынка: " + regime, 0.0});
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level,
        uncertainty_level == "Low" ? 0.2 : (uncertainty_level == "Extreme" ? -0.9 : -0.1)});

    record.verdict = "Сделка совершена";
    record.human_summary = generate_human_summary(record);
    record.machine_json = generate_machine_json(record);

    return record;
}

// ============================================================
// Объяснение отказа от сделки
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::explain_denial(
    const decision::DecisionRecord& decision,
    const risk::RiskDecision& risk_decision,
    const std::string& world_state,
    const std::string& regime,
    const std::string& uncertainty_level)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::TradeDenied;
    record.correlation_id = decision.correlation_id;
    record.symbol = decision.symbol;
    record.created_at = decision.decided_at;
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;
    record.trade_executed = false;

    if (decision.final_intent.has_value()) {
        record.strategy_id = decision.final_intent->strategy_id.get();
    }
    record.risk_verdict = risk_decision.summary;

    // Извлекаем причины отклонения
    extract_denial_factors(decision, risk_decision, record.factors);

    // Контекстные факторы
    record.factors.push_back({"world_model", "Состояние мира: " + world_state,
        world_state == "Disrupted" ? -0.8 : 0.0});
    record.factors.push_back({"regime", "Режим рынка: " + regime, 0.0});
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level,
        uncertainty_level == "Extreme" ? -0.9 : -0.1});

    record.verdict = "Сделка отклонена";
    record.human_summary = generate_human_summary(record);
    record.machine_json = generate_machine_json(record);

    return record;
}

// ============================================================
// Диагностика состояния системы
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_system_state(
    const std::string& world_state,
    const std::string& regime,
    const std::string& uncertainty_level,
    double portfolio_exposure_pct,
    double drawdown_pct,
    bool kill_switch_active)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::SystemState;
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;

    // Факторы состояния мира
    double world_impact = 0.0;
    if (world_state == "Stable") world_impact = 0.5;
    else if (world_state == "Transitioning") world_impact = -0.2;
    else if (world_state == "Disrupted") world_impact = -0.8;
    record.factors.push_back({"world_model", "Состояние мира: " + world_state, world_impact});

    // Режим рынка
    double regime_impact = 0.0;
    if (regime == "Trending") regime_impact = 0.3;
    else if (regime == "Volatile") regime_impact = -0.4;
    record.factors.push_back({"regime", "Режим рынка: " + regime, regime_impact});

    // Неопределённость
    double unc_impact = 0.0;
    if (uncertainty_level == "Low") unc_impact = 0.3;
    else if (uncertainty_level == "Moderate") unc_impact = 0.0;
    else if (uncertainty_level == "High") unc_impact = -0.5;
    else if (uncertainty_level == "Extreme") unc_impact = -0.9;
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level, unc_impact});

    // Экспозиция портфеля
    double exp_impact = portfolio_exposure_pct > 80.0 ? -0.6 : (portfolio_exposure_pct > 50.0 ? -0.2 : 0.1);
    record.factors.push_back({"portfolio",
        "Экспозиция: " + std::to_string(static_cast<int>(portfolio_exposure_pct)) + "%", exp_impact});

    // Просадка
    double dd_impact = drawdown_pct > 5.0 ? -0.8 : (drawdown_pct > 2.0 ? -0.4 : 0.0);
    record.factors.push_back({"portfolio",
        "Просадка: " + std::to_string(static_cast<int>(drawdown_pct)) + "%", dd_impact});

    // Аварийный выключатель
    if (kill_switch_active) {
        record.factors.push_back({"risk", "KILL SWITCH АКТИВЕН", -1.0});
        record.type = DiagnosticType::DegradedState;
    }

    // Определяем деградированное состояние
    if (world_state == "Disrupted" || uncertainty_level == "Extreme" || drawdown_pct > 5.0) {
        record.type = DiagnosticType::DegradedState;
    }

    record.verdict = (record.type == DiagnosticType::DegradedState)
        ? "Система в деградированном состоянии"
        : "Система работает нормально";

    record.human_summary = generate_human_summary(record);
    record.machine_json = generate_machine_json(record);

    return record;
}

// ============================================================
// Генерация человекочитаемого резюме
// ============================================================

std::string SelfDiagnosisEngine::generate_human_summary(const DiagnosticRecord& record) {
    std::ostringstream ss;

    switch (record.type) {
        case DiagnosticType::TradeTaken:
            ss << "Сделка совершена";
            if (!record.strategy_id.empty()) {
                ss << " по стратегии [" << record.strategy_id << "]";
            }
            ss << ". Режим: " << record.regime
               << ". Мир: " << record.world_state
               << ". Неопределённость: " << record.uncertainty_level
               << ". Риск: одобрено.";
            break;

        case DiagnosticType::TradeDenied: {
            ss << "Сделка отклонена. Причины: ";
            bool first = true;
            for (const auto& f : record.factors) {
                if (f.impact < -0.3) {
                    if (!first) ss << "; ";
                    ss << f.observation;
                    first = false;
                }
            }
            if (first) ss << "нет критических факторов";
            ss << ". Риск-вердикт: " << record.risk_verdict << ".";
            break;
        }

        case DiagnosticType::SystemState:
        case DiagnosticType::DegradedState:
            ss << record.verdict << ". "
               << "Мир: " << record.world_state
               << ", режим: " << record.regime
               << ", неопределённость: " << record.uncertainty_level << ".";
            if (record.type == DiagnosticType::DegradedState) {
                ss << " Внимание: обнаружены деградированные факторы.";
            }
            break;

        case DiagnosticType::StrategyHealth:
            ss << "Здоровье стратегии: " << record.strategy_id << ".";
            break;
    }

    return ss.str();
}

// ============================================================
// Генерация машиночитаемого JSON
// ============================================================

std::string SelfDiagnosisEngine::generate_machine_json(const DiagnosticRecord& record) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"diagnostic_id\":" << record.diagnostic_id;
    ss << ",\"type\":\"" << to_string(record.type) << "\"";
    ss << ",\"correlation_id\":\"" << record.correlation_id.get() << "\"";
    ss << ",\"symbol\":\"" << record.symbol.get() << "\"";
    ss << ",\"world_state\":\"" << record.world_state << "\"";
    ss << ",\"regime\":\"" << record.regime << "\"";
    ss << ",\"uncertainty_level\":\"" << record.uncertainty_level << "\"";
    ss << ",\"verdict\":\"" << record.verdict << "\"";
    ss << ",\"trade_executed\":" << (record.trade_executed ? "true" : "false");
    ss << ",\"strategy_id\":\"" << record.strategy_id << "\"";
    ss << ",\"risk_verdict\":\"" << record.risk_verdict << "\"";

    ss << ",\"factors\":[";
    for (std::size_t i = 0; i < record.factors.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"component\":\"" << record.factors[i].component << "\""
           << ",\"observation\":\"" << record.factors[i].observation << "\""
           << ",\"impact\":" << record.factors[i].impact << "}";
    }
    ss << "]";

    ss << "}";
    return ss.str();
}

// ============================================================
// Вспомогательные методы извлечения факторов
// ============================================================

void SelfDiagnosisEngine::extract_decision_factors(
    const decision::DecisionRecord& decision,
    std::vector<DiagnosticFactor>& factors)
{
    // Факторы из вкладов стратегий
    for (const auto& contrib : decision.contributions) {
        double impact = 0.0;
        std::string obs;

        if (contrib.intent.has_value()) {
            impact = contrib.intent->conviction * contrib.weight;
            obs = "Стратегия " + contrib.strategy_id.get() +
                  ": уверенность " + std::to_string(static_cast<int>(contrib.intent->conviction * 100)) +
                  "%, вес " + std::to_string(static_cast<int>(contrib.weight * 100)) + "%";
        } else {
            obs = "Стратегия " + contrib.strategy_id.get() + ": нет сигнала";
        }

        if (contrib.was_vetoed) {
            impact = -0.5;
            obs += " [ВЕТО]";
        }

        factors.push_back({"strategy:" + contrib.strategy_id.get(), obs, impact});
    }

    // Итоговая уверенность
    factors.push_back({"decision",
        "Итоговая уверенность: " + std::to_string(static_cast<int>(decision.final_conviction * 100)) + "%",
        decision.final_conviction});
}

void SelfDiagnosisEngine::extract_risk_factors(
    const risk::RiskDecision& risk_decision,
    std::vector<DiagnosticFactor>& factors)
{
    std::string verdict_str;
    double impact = 0.0;

    switch (risk_decision.verdict) {
        case risk::RiskVerdict::Approved:
            verdict_str = "Одобрено";
            impact = 0.3;
            break;
        case risk::RiskVerdict::ReduceSize:
            verdict_str = "Одобрено с уменьшением";
            impact = 0.1;
            break;
        case risk::RiskVerdict::Denied:
            verdict_str = "Отклонено";
            impact = -1.0;
            break;
        case risk::RiskVerdict::Throttled:
            verdict_str = "Отложено";
            impact = -0.5;
            break;
    }

    factors.push_back({"risk", "Риск-вердикт: " + verdict_str, impact});

    // Причины риск-решения
    for (const auto& reason : risk_decision.reasons) {
        factors.push_back({"risk:" + reason.code, reason.message, -reason.severity});
    }
}

void SelfDiagnosisEngine::extract_denial_factors(
    const decision::DecisionRecord& decision,
    const risk::RiskDecision& risk_decision,
    std::vector<DiagnosticFactor>& factors)
{
    // Глобальные вето из решения
    for (const auto& veto : decision.global_vetoes) {
        factors.push_back({"veto:" + veto.source, veto.reason, -veto.severity});
    }

    // Вето из вкладов стратегий
    for (const auto& contrib : decision.contributions) {
        for (const auto& vr : contrib.veto_reasons) {
            factors.push_back({"strategy_veto:" + vr.source, vr.reason, -vr.severity});
        }
    }

    // Причины из риск-движка
    for (const auto& reason : risk_decision.reasons) {
        factors.push_back({"risk:" + reason.code, reason.message, -reason.severity});
    }

    // Риск-вердикт
    std::string verdict_str;
    switch (risk_decision.verdict) {
        case risk::RiskVerdict::Denied:    verdict_str = "Отклонено"; break;
        case risk::RiskVerdict::Throttled: verdict_str = "Отложено"; break;
        default:                           verdict_str = "Прочее"; break;
    }
    factors.push_back({"risk", "Риск-вердикт: " + verdict_str, -0.8});
}

} // namespace tb::self_diagnosis
