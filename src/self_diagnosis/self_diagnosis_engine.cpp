/**
 * @file self_diagnosis_engine.cpp
 * @brief Реализация движка самодиагностики
 *
 * Формирует объяснения решений: почему сделка совершена или отклонена,
 * диагностирует состояние системы, ошибки исполнения, деградацию данных,
 * проблемы подключения, ограничения портфеля и действия восстановления.
 *
 * Каждая диагностическая запись:
 *   - получает severity и recommended_action автоматически
 *   - персистируется в EventJournal (если подключён)
 *   - обновляет агрегированные scorecards по стратегиям
 *   - публикует метрики Prometheus (если реестр подключён)
 */
#include "self_diagnosis/self_diagnosis_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "metrics/counter.hpp"
#include "metrics/gauge.hpp"
#include "persistence/event_journal.hpp"
#include "persistence/persistence_types.hpp"
#include <boost/json.hpp>
#include <sstream>
#include <stdexcept>

namespace tb::self_diagnosis {

// ============================================================
// Конструкторы
// ============================================================

SelfDiagnosisEngine::SelfDiagnosisEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<persistence::EventJournal> journal)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , journal_(std::move(journal))
{
    if (!logger_) {
        throw std::invalid_argument("SelfDiagnosisEngine: logger не может быть null");
    }
    if (!clock_) {
        throw std::invalid_argument("SelfDiagnosisEngine: clock не может быть null");
    }
    if (metrics_) {
        counter_diagnostics_total_ = metrics_->counter("tb_diag_records_total", {});
        gauge_severity_level_ = metrics_->gauge("tb_diag_max_severity", {});
    }
}

SelfDiagnosisEngine::SelfDiagnosisEngine() = default;

uint64_t SelfDiagnosisEngine::next_diagnostic_id() {
    // HIGH-14: acq_rel prevents duplicate IDs under concurrent access by
    // establishing a happens-before relationship across threads.
    return next_id_.fetch_add(1, std::memory_order_acq_rel);
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
    record.created_at = clock_ ? clock_->now() : decision.decided_at;
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;
    record.trade_executed = true;

    if (decision.final_intent.has_value()) {
        record.strategy_id = decision.final_intent->strategy_id.get();
    }
    record.risk_verdict = risk_decision.summary;

    extract_decision_factors(decision, record.factors);
    extract_risk_factors(risk_decision, record.factors);

    record.factors.push_back({"world_model", "Состояние мира: " + world_state,
        world_state == "Stable" ? 0.3 : (world_state == "Disrupted" ? -0.8 : 0.0)});
    record.factors.push_back({"regime", "Режим рынка: " + regime, 0.0});
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level,
        uncertainty_level == "Low" ? 0.2 : (uncertainty_level == "Extreme" ? -0.9 : -0.1)});

    record.verdict = "Сделка совершена";
    finalize_record(record);

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
    record.created_at = clock_ ? clock_->now() : decision.decided_at;
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;
    record.trade_executed = false;

    if (decision.final_intent.has_value()) {
        record.strategy_id = decision.final_intent->strategy_id.get();
    }
    record.risk_verdict = risk_decision.summary;

    extract_denial_factors(decision, risk_decision, record.factors);

    record.factors.push_back({"world_model", "Состояние мира: " + world_state,
        world_state == "Disrupted" ? -0.8 : 0.0});
    record.factors.push_back({"regime", "Режим рынка: " + regime, 0.0});
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level,
        uncertainty_level == "Extreme" ? -0.9 : -0.1});

    record.verdict = "Сделка отклонена";
    finalize_record(record);

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
    record.created_at = clock_ ? clock_->now() : Timestamp{0};
    record.world_state = world_state;
    record.regime = regime;
    record.uncertainty_level = uncertainty_level;

    double world_impact = 0.0;
    if (world_state == "Stable") world_impact = 0.5;
    else if (world_state == "Transitioning") world_impact = -0.2;
    else if (world_state == "Disrupted") world_impact = -0.8;
    record.factors.push_back({"world_model", "Состояние мира: " + world_state, world_impact});

    double regime_impact = 0.0;
    if (regime == "Trending") regime_impact = 0.3;
    else if (regime == "Volatile") regime_impact = -0.4;
    record.factors.push_back({"regime", "Режим рынка: " + regime, regime_impact});

    double unc_impact = 0.0;
    if (uncertainty_level == "Low") unc_impact = 0.3;
    else if (uncertainty_level == "Moderate") unc_impact = 0.0;
    else if (uncertainty_level == "High") unc_impact = -0.5;
    else if (uncertainty_level == "Extreme") unc_impact = -0.9;
    record.factors.push_back({"uncertainty", "Неопределённость: " + uncertainty_level, unc_impact});

    double exp_impact = portfolio_exposure_pct > 80.0 ? -0.6 : (portfolio_exposure_pct > 50.0 ? -0.2 : 0.1);
    record.factors.push_back({"portfolio",
        "Экспозиция: " + std::to_string(static_cast<int>(portfolio_exposure_pct)) + "%", exp_impact});

    double dd_impact = drawdown_pct > 5.0 ? -0.8 : (drawdown_pct > 2.0 ? -0.4 : 0.0);
    record.factors.push_back({"portfolio",
        "Просадка: " + std::to_string(static_cast<int>(drawdown_pct)) + "%", dd_impact});

    if (kill_switch_active) {
        record.factors.push_back({"risk", "KILL SWITCH АКТИВЕН", -1.0});
        record.type = DiagnosticType::DegradedState;
    }

    if (world_state == "Disrupted" || uncertainty_level == "Extreme" || drawdown_pct > 5.0) {
        record.type = DiagnosticType::DegradedState;
    }

    record.verdict = (record.type == DiagnosticType::DegradedState)
        ? "Система в деградированном состоянии"
        : "Система работает нормально";

    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика ошибки исполнения ордера
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_execution_failure(
    const Symbol& symbol,
    const StrategyId& strategy_id,
    const CorrelationId& correlation_id,
    const std::string& error_message,
    int consecutive_failures)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::ExecutionFailure;
    record.symbol = symbol;
    record.strategy_id = strategy_id.get();
    record.correlation_id = correlation_id;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double failure_impact = consecutive_failures >= 5 ? -1.0
                          : consecutive_failures >= 3 ? -0.7
                          : -0.4;
    record.factors.push_back({"execution",
        "Ошибка исполнения: " + error_message, failure_impact});
    record.factors.push_back({"execution",
        "Подряд ошибок: " + std::to_string(consecutive_failures),
        static_cast<double>(-consecutive_failures) / 10.0});

    record.verdict = "Ошибка исполнения ордера";
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика деградации рыночных данных
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_market_data_degradation(
    const Symbol& symbol,
    const std::string& degradation_type,
    int64_t staleness_ns,
    double spread_bps)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::MarketDataDegradation;
    record.symbol = symbol;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double staleness_ms = static_cast<double>(staleness_ns) / 1'000'000.0;
    double stale_impact = staleness_ms > 10000.0 ? -0.9
                        : staleness_ms > 5000.0  ? -0.6
                        : staleness_ms > 1000.0  ? -0.3
                        : 0.0;
    record.factors.push_back({"market_data",
        "Тип деградации: " + degradation_type, stale_impact});
    record.factors.push_back({"market_data",
        "Устаревание: " + std::to_string(static_cast<int>(staleness_ms)) + "мс",
        stale_impact});

    double spread_impact = spread_bps > 100.0 ? -0.8
                         : spread_bps > 50.0  ? -0.5
                         : spread_bps > 20.0  ? -0.2
                         : 0.0;
    record.factors.push_back({"market_data",
        "Спред: " + std::to_string(static_cast<int>(spread_bps)) + " bps",
        spread_impact});

    record.verdict = "Деградация рыночных данных: " + degradation_type;
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика инцидента подключения к бирже
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_connectivity_incident(
    const std::string& endpoint,
    const std::string& error_message,
    int reconnect_attempt)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::ExchangeConnectivityIncident;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double attempt_impact = reconnect_attempt >= 10 ? -1.0
                          : reconnect_attempt >= 5  ? -0.7
                          : reconnect_attempt >= 2  ? -0.4
                          : -0.2;
    record.factors.push_back({"connectivity",
        "Эндпоинт: " + endpoint, attempt_impact});
    record.factors.push_back({"connectivity",
        "Ошибка: " + error_message, attempt_impact});
    record.factors.push_back({"connectivity",
        "Попытка переподключения: " + std::to_string(reconnect_attempt),
        attempt_impact});

    record.verdict = "Инцидент подключения к бирже";
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика подавления стратегии
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_strategy_suppression(
    const StrategyId& strategy_id,
    const std::string& suppression_source,
    const std::string& reason,
    double alpha_decay_score)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::StrategySuppression;
    record.strategy_id = strategy_id.get();
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double decay_impact = alpha_decay_score < 0.2 ? -0.9
                        : alpha_decay_score < 0.5 ? -0.5
                        : -0.2;
    record.factors.push_back({"strategy",
        "Источник подавления: " + suppression_source, decay_impact});
    record.factors.push_back({"strategy",
        "Причина: " + reason, decay_impact});
    record.factors.push_back({"alpha_decay",
        "Alpha decay score: " + std::to_string(static_cast<int>(alpha_decay_score * 100)) + "%",
        decay_impact});

    record.verdict = "Стратегия подавлена: " + strategy_id.get();
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика ограничения портфеля
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_portfolio_constraint(
    const Symbol& symbol,
    const std::string& constraint_type,
    double current_value,
    double limit_value)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::PortfolioConstraint;
    record.symbol = symbol;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double ratio = (limit_value > 0.0) ? current_value / limit_value : 1.0;
    double constraint_impact = ratio >= 1.0 ? -0.8
                             : ratio >= 0.9 ? -0.5
                             : ratio >= 0.8 ? -0.3
                             : 0.0;
    record.factors.push_back({"portfolio",
        "Ограничение: " + constraint_type, constraint_impact});
    record.factors.push_back({"portfolio",
        "Текущее: " + std::to_string(current_value) + ", лимит: " + std::to_string(limit_value),
        constraint_impact});

    record.verdict = "Ограничение портфеля: " + constraint_type;
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика действия восстановления
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_recovery_action(
    const std::string& action_description,
    const std::string& trigger_reason,
    bool success)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::RecoveryAction;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    double success_impact = success ? 0.5 : -0.6;
    record.factors.push_back({"recovery",
        "Действие: " + action_description, success_impact});
    record.factors.push_back({"recovery",
        "Триггер: " + trigger_reason, 0.0});
    record.factors.push_back({"recovery",
        success ? "Результат: успешно" : "Результат: неудача",
        success_impact});

    record.verdict = success
        ? "Восстановление выполнено: " + action_description
        : "Восстановление не удалось: " + action_description;
    finalize_record(record);

    return record;
}

// ============================================================
// Диагностика расхождения при сверке с биржей
// ============================================================

DiagnosticRecord SelfDiagnosisEngine::diagnose_reconciliation_mismatch(
    const Symbol& symbol,
    const std::string& mismatch_type,
    const std::string& local_state,
    const std::string& exchange_state)
{
    std::lock_guard lock(mutex_);

    DiagnosticRecord record;
    record.diagnostic_id = next_diagnostic_id();
    record.type = DiagnosticType::ReconciliationMismatch;
    record.symbol = symbol;
    record.created_at = clock_ ? clock_->now() : Timestamp{0};

    record.factors.push_back({"reconciliation",
        "Тип расхождения: " + mismatch_type, -0.7});
    record.factors.push_back({"reconciliation",
        "Локальное: " + local_state, 0.0});
    record.factors.push_back({"reconciliation",
        "Биржевое: " + exchange_state, 0.0});

    record.verdict = "Расхождение при сверке: " + mismatch_type;
    finalize_record(record);

    return record;
}

// ============================================================
// Scorecards и агрегация
// ============================================================

DiagnosticSummary SelfDiagnosisEngine::get_summary() const {
    std::lock_guard lock(mutex_);
    return summary_;
}

StrategyScorecard SelfDiagnosisEngine::get_strategy_scorecard(const std::string& strategy_id) const {
    std::lock_guard lock(mutex_);
    auto it = summary_.strategy_scorecards.find(strategy_id);
    if (it != summary_.strategy_scorecards.end()) {
        return it->second;
    }
    StrategyScorecard empty;
    empty.strategy_id = strategy_id;
    return empty;
}

std::vector<DiagnosticRecord> SelfDiagnosisEngine::get_recent_records(std::size_t max_count) const {
    std::lock_guard lock(mutex_);
    std::size_t count = std::min(max_count, recent_records_.size());
    return {recent_records_.end() - static_cast<std::ptrdiff_t>(count), recent_records_.end()};
}

void SelfDiagnosisEngine::reset_scorecards() {
    std::lock_guard lock(mutex_);
    summary_ = DiagnosticSummary{};
    recent_records_.clear();

    if (logger_) {
        logger_->info("self_diagnosis", "Scorecards сброшены");
    }
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

        case DiagnosticType::ExecutionFailure:
            ss << "Ошибка исполнения";
            if (record.symbol.get().size() > 0) {
                ss << " [" << record.symbol.get() << "]";
            }
            ss << ": " << record.verdict
               << ". Действие: " << to_string(record.recommended_action) << ".";
            break;

        case DiagnosticType::MarketDataDegradation:
            ss << "Деградация данных";
            if (record.symbol.get().size() > 0) {
                ss << " [" << record.symbol.get() << "]";
            }
            ss << ": " << record.verdict << ".";
            break;

        case DiagnosticType::ExchangeConnectivityIncident:
            ss << "Инцидент подключения: " << record.verdict
               << ". Действие: " << to_string(record.recommended_action) << ".";
            break;

        case DiagnosticType::StrategySuppression:
            ss << "Подавление стратегии [" << record.strategy_id
               << "]: " << record.verdict << ".";
            break;

        case DiagnosticType::PortfolioConstraint:
            ss << "Ограничение портфеля";
            if (record.symbol.get().size() > 0) {
                ss << " [" << record.symbol.get() << "]";
            }
            ss << ": " << record.verdict << ".";
            break;

        case DiagnosticType::RecoveryAction:
            ss << "Восстановление: " << record.verdict << ".";
            break;

        case DiagnosticType::ReconciliationMismatch:
            ss << "Расхождение при сверке";
            if (record.symbol.get().size() > 0) {
                ss << " [" << record.symbol.get() << "]";
            }
            ss << ": " << record.verdict
               << ". Действие: " << to_string(record.recommended_action) << ".";
            break;
    }

    return ss.str();
}

// ============================================================
// Генерация машиночитаемого JSON
// ============================================================

std::string SelfDiagnosisEngine::generate_machine_json(const DiagnosticRecord& record) {
    // CRITICAL-9: Use boost::json::object to properly escape all string values.
    // Raw string concatenation would produce broken JSON if symbol/verdict/etc
    // contain '"' or '\' characters.
    boost::json::object obj;
    obj["diagnostic_id"]       = record.diagnostic_id;
    obj["type"]                = to_string(record.type);
    obj["severity"]            = to_string(record.severity);
    obj["recommended_action"]  = to_string(record.recommended_action);
    obj["correlation_id"]      = record.correlation_id.get();
    obj["symbol"]              = record.symbol.get();
    obj["created_at"]          = record.created_at.get();
    obj["world_state"]         = record.world_state;
    obj["regime"]              = record.regime;
    obj["uncertainty_level"]   = record.uncertainty_level;
    obj["verdict"]             = record.verdict;
    obj["trade_executed"]      = record.trade_executed;
    obj["strategy_id"]         = record.strategy_id;
    obj["risk_verdict"]        = record.risk_verdict;

    boost::json::array factors_arr;
    for (const auto& f : record.factors) {
        boost::json::object factor_obj;
        factor_obj["component"]   = f.component;
        factor_obj["observation"] = f.observation;
        factor_obj["impact"]      = f.impact;
        factors_arr.push_back(std::move(factor_obj));
    }
    obj["factors"] = std::move(factors_arr);

    return boost::json::serialize(obj);
}

// ============================================================
// Вспомогательные методы извлечения факторов
// ============================================================

void SelfDiagnosisEngine::extract_decision_factors(
    const decision::DecisionRecord& decision,
    std::vector<DiagnosticFactor>& factors)
{
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

    for (const auto& reason : risk_decision.reasons) {
        factors.push_back({"risk:" + reason.code, reason.message, -reason.severity});
    }
}

void SelfDiagnosisEngine::extract_denial_factors(
    const decision::DecisionRecord& decision,
    const risk::RiskDecision& risk_decision,
    std::vector<DiagnosticFactor>& factors)
{
    for (const auto& veto : decision.global_vetoes) {
        factors.push_back({"veto:" + veto.source, veto.reason, -veto.severity});
    }

    for (const auto& contrib : decision.contributions) {
        for (const auto& vr : contrib.veto_reasons) {
            factors.push_back({"strategy_veto:" + vr.source, vr.reason, -vr.severity});
        }
    }

    for (const auto& reason : risk_decision.reasons) {
        factors.push_back({"risk:" + reason.code, reason.message, -reason.severity});
    }

    std::string verdict_str;
    switch (risk_decision.verdict) {
        case risk::RiskVerdict::Denied:    verdict_str = "Отклонено"; break;
        case risk::RiskVerdict::Throttled: verdict_str = "Отложено"; break;
        default:                           verdict_str = "Прочее"; break;
    }
    factors.push_back({"risk", "Риск-вердикт: " + verdict_str, -0.8});
}

// ============================================================
// Вычисление серьёзности
// ============================================================

DiagnosticSeverity SelfDiagnosisEngine::compute_severity(const DiagnosticRecord& record) {
    // Фатальные типы
    if (record.type == DiagnosticType::ReconciliationMismatch) {
        return DiagnosticSeverity::Critical;
    }

    // Определяем по наихудшему факторному воздействию
    double worst_impact = 0.0;
    for (const auto& f : record.factors) {
        if (f.impact < worst_impact) {
            worst_impact = f.impact;
        }
    }

    if (worst_impact <= -0.9) return DiagnosticSeverity::Fatal;
    if (worst_impact <= -0.6) return DiagnosticSeverity::Critical;
    if (worst_impact <= -0.3) return DiagnosticSeverity::Warning;
    return DiagnosticSeverity::Info;
}

// ============================================================
// Вычисление рекомендуемого корректирующего действия
// ============================================================

CorrectiveAction SelfDiagnosisEngine::compute_corrective_action(const DiagnosticRecord& record) {
    switch (record.type) {
        case DiagnosticType::ReconciliationMismatch:
            return CorrectiveAction::ForceReconcile;

        case DiagnosticType::ExecutionFailure:
            if (record.severity == DiagnosticSeverity::Fatal) {
                return CorrectiveAction::HaltSymbol;
            }
            if (record.severity == DiagnosticSeverity::Critical) {
                return CorrectiveAction::StopEntries;
            }
            return CorrectiveAction::SlowDown;

        case DiagnosticType::ExchangeConnectivityIncident:
            if (record.severity >= DiagnosticSeverity::Critical) {
                return CorrectiveAction::HaltSystem;
            }
            return CorrectiveAction::StopEntries;

        case DiagnosticType::MarketDataDegradation:
            if (record.severity >= DiagnosticSeverity::Critical) {
                return CorrectiveAction::HaltSymbol;
            }
            return CorrectiveAction::SlowDown;

        case DiagnosticType::DegradedState:
            if (record.severity >= DiagnosticSeverity::Fatal) {
                return CorrectiveAction::HaltSystem;
            }
            return CorrectiveAction::ReduceSize;

        case DiagnosticType::PortfolioConstraint:
            return CorrectiveAction::ReduceSize;

        case DiagnosticType::StrategySuppression:
            return CorrectiveAction::Observe;

        case DiagnosticType::RecoveryAction:
            return CorrectiveAction::Observe;

        case DiagnosticType::TradeTaken:
        case DiagnosticType::TradeDenied:
        case DiagnosticType::SystemState:
        case DiagnosticType::StrategyHealth:
            return CorrectiveAction::Observe;
    }
    return CorrectiveAction::Observe;
}

// ============================================================
// Финализация записи: severity, action, summary, json, persist
// ============================================================

void SelfDiagnosisEngine::finalize_record(DiagnosticRecord& record) {
    record.severity = compute_severity(record);
    record.recommended_action = compute_corrective_action(record);
    record.human_summary = generate_human_summary(record);
    record.machine_json = generate_machine_json(record);

    // Сохраняем в in-memory буфер
    recent_records_.push_back(record);
    if (recent_records_.size() > kMaxRecentRecords) {
        recent_records_.pop_front();
    }

    update_summary(record);
    update_metrics(record);
    persist_record(record);

    // Логируем значимые события
    if (logger_ && record.severity >= DiagnosticSeverity::Warning) {
        auto log_fn = (record.severity >= DiagnosticSeverity::Critical)
            ? &logging::ILogger::error
            : &logging::ILogger::warn;

        (logger_.get()->*log_fn)("self_diagnosis", record.human_summary, {
            {"diagnostic_id", std::to_string(record.diagnostic_id)},
            {"type", to_string(record.type)},
            {"severity", to_string(record.severity)},
            {"action", to_string(record.recommended_action)},
            {"symbol", record.symbol.get()},
            {"strategy_id", record.strategy_id}
        });
    }
}

// ============================================================
// Персистентность (best-effort)
// ============================================================

void SelfDiagnosisEngine::persist_record(const DiagnosticRecord& record) {
    if (!journal_) return;

    auto result = journal_->append(
        persistence::JournalEntryType::DiagnosticEvent,
        record.machine_json,
        record.correlation_id,
        StrategyId{record.strategy_id},
        ConfigHash{""});

    if (!result && logger_) {
        logger_->warn("self_diagnosis",
            "Не удалось персистировать диагностику #" +
            std::to_string(record.diagnostic_id));
    }
}

// ============================================================
// Обновление агрегированной статистики
// ============================================================

void SelfDiagnosisEngine::update_summary(const DiagnosticRecord& record) {
    summary_.total_records++;

    switch (record.severity) {
        case DiagnosticSeverity::Info:     summary_.info_count++; break;
        case DiagnosticSeverity::Warning:  summary_.warning_count++; break;
        case DiagnosticSeverity::Critical: summary_.critical_count++; break;
        case DiagnosticSeverity::Fatal:    summary_.fatal_count++; break;
    }

    summary_.type_counts[to_string(record.type)]++;

    // Обновляем scorecard стратегии
    if (!record.strategy_id.empty()) {
        auto& sc = summary_.strategy_scorecards[record.strategy_id];
        sc.strategy_id = record.strategy_id;
        sc.total_signals++;

        switch (record.type) {
            case DiagnosticType::TradeTaken:
                sc.trades_taken++;
                break;
            case DiagnosticType::TradeDenied:
                sc.trades_denied++;
                break;
            case DiagnosticType::ExecutionFailure:
                sc.execution_failures++;
                break;
            case DiagnosticType::StrategySuppression:
                sc.suppressions++;
                break;
            default:
                break;
        }

        // Обновляем denial_rate
        uint64_t total_decisions = sc.trades_taken + sc.trades_denied;
        sc.denial_rate = (total_decisions > 0)
            ? static_cast<double>(sc.trades_denied) / static_cast<double>(total_decisions)
            : 0.0;
    }
}

// ============================================================
// Обновление метрик Prometheus
// ============================================================

void SelfDiagnosisEngine::update_metrics(const DiagnosticRecord& record) {
    if (!counter_diagnostics_total_) return;

    counter_diagnostics_total_->increment();

    if (gauge_severity_level_) {
        double severity_val = 0.0;
        switch (record.severity) {
            case DiagnosticSeverity::Info:     severity_val = 0.0; break;
            case DiagnosticSeverity::Warning:  severity_val = 1.0; break;
            case DiagnosticSeverity::Critical: severity_val = 2.0; break;
            case DiagnosticSeverity::Fatal:    severity_val = 3.0; break;
        }
        if (severity_val > gauge_severity_level_->value()) {
            gauge_severity_level_->set(severity_val);
        }
    }
}

} // namespace tb::self_diagnosis
