#pragma once
/**
 * @file self_diagnosis_engine.hpp
 * @brief Движок самодиагностики — объяснение решений, корректирующие действия и scorecards
 *
 * Принимает контекст из различных модулей и формирует полные
 * объяснения принятых/отклонённых сделок, диагностирует состояние системы,
 * генерирует рекомендуемые корректирующие действия и ведёт агрегированные scorecards.
 *
 * Интеграция:
 *   - Вызывается из TradingPipeline на всех ключевых исходах
 *   - Записи персистируются в EventJournal (если подключён)
 *   - Корректирующие действия могут транслироваться в governance/risk
 */
#include "self_diagnosis/self_diagnosis_types.hpp"
#include "decision/decision_types.hpp"
#include "risk/risk_types.hpp"
#include "common/result.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace tb::logging { class ILogger; }
namespace tb::clock { class IClock; }
namespace tb::metrics { class IMetricsRegistry; class ICounter; class IGauge; }
namespace tb::persistence { class EventJournal; }

namespace tb::self_diagnosis {

class SelfDiagnosisEngine {
public:
    /// @param logger   Логгер (обязателен)
    /// @param clock    Часы для меток времени (обязательны)
    /// @param metrics  Реестр метрик (опционален)
    /// @param journal  Журнал событий для персистентности (опционален)
    SelfDiagnosisEngine(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr,
        std::shared_ptr<persistence::EventJournal> journal = nullptr);

    /// Конструктор по умолчанию (для обратной совместимости и тестов)
    SelfDiagnosisEngine();

    // ============================================================
    // Основные диагностические методы (существующие + расширенные)
    // ============================================================

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

    // ============================================================
    // Новые диагностические методы (Phase 3)
    // ============================================================

    /// Диагностика ошибки исполнения ордера
    [[nodiscard]] DiagnosticRecord diagnose_execution_failure(
        const Symbol& symbol,
        const StrategyId& strategy_id,
        const CorrelationId& correlation_id,
        const std::string& error_message,
        int consecutive_failures);

    /// Диагностика деградации рыночных данных
    [[nodiscard]] DiagnosticRecord diagnose_market_data_degradation(
        const Symbol& symbol,
        const std::string& degradation_type,
        int64_t staleness_ns,
        double spread_bps);

    /// Диагностика инцидента подключения к бирже
    [[nodiscard]] DiagnosticRecord diagnose_connectivity_incident(
        const std::string& endpoint,
        const std::string& error_message,
        int reconnect_attempt);

    /// Диагностика подавления стратегии
    [[nodiscard]] DiagnosticRecord diagnose_strategy_suppression(
        const StrategyId& strategy_id,
        const std::string& suppression_source,
        const std::string& reason,
        double alpha_decay_score);

    /// Диагностика ограничения портфеля
    [[nodiscard]] DiagnosticRecord diagnose_portfolio_constraint(
        const Symbol& symbol,
        const std::string& constraint_type,
        double current_value,
        double limit_value);

    /// Диагностика действия восстановления
    [[nodiscard]] DiagnosticRecord diagnose_recovery_action(
        const std::string& action_description,
        const std::string& trigger_reason,
        bool success);

    /// Диагностика расхождения при сверке с биржей
    [[nodiscard]] DiagnosticRecord diagnose_reconciliation_mismatch(
        const Symbol& symbol,
        const std::string& mismatch_type,
        const std::string& local_state,
        const std::string& exchange_state);

    // ============================================================
    // Scorecards и агрегация
    // ============================================================

    /// Получить агрегированную сводку диагностики
    [[nodiscard]] DiagnosticSummary get_summary() const;

    /// Получить scorecard по конкретной стратегии
    [[nodiscard]] StrategyScorecard get_strategy_scorecard(const std::string& strategy_id) const;

    /// Получить последние N диагностических записей
    [[nodiscard]] std::vector<DiagnosticRecord> get_recent_records(std::size_t max_count = 100) const;

    /// Сбросить счётчики scorecards (вызывается при дневной ротации)
    void reset_scorecards();

    // ============================================================
    // Генерация отчётов (обратная совместимость)
    // ============================================================

    /// Сгенерировать человекочитаемое резюме
    [[nodiscard]] static std::string generate_human_summary(const DiagnosticRecord& record);

    /// Сгенерировать машиночитаемый JSON
    [[nodiscard]] static std::string generate_machine_json(const DiagnosticRecord& record);

private:
    std::atomic<uint64_t> next_id_{1};
    mutable std::mutex mutex_;

    // Зависимости (опциональные — для обратной совместимости с тестами)
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<persistence::EventJournal> journal_;

    // In-memory буфер последних записей
    std::deque<DiagnosticRecord> recent_records_;
    static constexpr std::size_t kMaxRecentRecords = 1000;

    // Агрегированная статистика
    DiagnosticSummary summary_;

    // Кэши метрик
    std::shared_ptr<metrics::ICounter> counter_diagnostics_total_;
    std::shared_ptr<metrics::IGauge> gauge_severity_level_;

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

    /// Определить серьёзность события по факторам
    static DiagnosticSeverity compute_severity(const DiagnosticRecord& record);

    /// Определить рекомендуемое корректирующее действие
    static CorrectiveAction compute_corrective_action(const DiagnosticRecord& record);

    /// Финализация записи: severity, action, summary, json, persist, scorecard
    void finalize_record(DiagnosticRecord& record);

    /// Персистировать запись в EventJournal (best-effort)
    void persist_record(const DiagnosticRecord& record);

    /// Обновить агрегированную статистику
    void update_summary(const DiagnosticRecord& record);

    /// Обновить метрики Prometheus
    void update_metrics(const DiagnosticRecord& record);
};

} // namespace tb::self_diagnosis
