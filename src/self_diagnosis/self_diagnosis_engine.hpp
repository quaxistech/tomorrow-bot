#pragma once
/**
 * @file self_diagnosis_engine.hpp
 * @brief Движок самодиагностики — объяснение решений и состояния системы
 *
 * Принимает контекст из различных модулей и формирует полные
 * объяснения принятых/отклонённых сделок и состояния системы.
 */
#include "self_diagnosis/self_diagnosis_types.hpp"
#include "decision/decision_types.hpp"
#include "risk/risk_types.hpp"
#include "common/result.hpp"
#include <atomic>
#include <mutex>
#include <string>

namespace tb::self_diagnosis {

class SelfDiagnosisEngine {
public:
    SelfDiagnosisEngine();

    /// Объяснить совершённую сделку
    [[nodiscard]] DiagnosticRecord explain_trade(
        const decision::DecisionRecord& decision,
        const risk::RiskDecision& risk_decision,
        const std::string& world_state,
        const std::string& regime,
        const std::string& uncertainty_level);

    /// Объяснить отказ от сделки
    [[nodiscard]] DiagnosticRecord explain_denial(
        const decision::DecisionRecord& decision,
        const risk::RiskDecision& risk_decision,
        const std::string& world_state,
        const std::string& regime,
        const std::string& uncertainty_level);

    /// Диагностика состояния системы
    [[nodiscard]] DiagnosticRecord diagnose_system_state(
        const std::string& world_state,
        const std::string& regime,
        const std::string& uncertainty_level,
        double portfolio_exposure_pct,
        double drawdown_pct,
        bool kill_switch_active);

    /// Сгенерировать человекочитаемое резюме
    [[nodiscard]] static std::string generate_human_summary(const DiagnosticRecord& record);

    /// Сгенерировать машиночитаемый JSON
    [[nodiscard]] static std::string generate_machine_json(const DiagnosticRecord& record);

private:
    std::atomic<uint64_t> next_id_{1};
    mutable std::mutex mutex_;

    /// Получить следующий уникальный ID диагностики
    [[nodiscard]] uint64_t next_diagnostic_id();

    /// Извлечь факторы из DecisionRecord
    static void extract_decision_factors(
        const decision::DecisionRecord& decision,
        std::vector<DiagnosticFactor>& factors);

    /// Извлечь факторы из RiskDecision
    static void extract_risk_factors(
        const risk::RiskDecision& risk_decision,
        std::vector<DiagnosticFactor>& factors);

    /// Извлечь причины отклонения
    static void extract_denial_factors(
        const decision::DecisionRecord& decision,
        const risk::RiskDecision& risk_decision,
        std::vector<DiagnosticFactor>& factors);
};

} // namespace tb::self_diagnosis
