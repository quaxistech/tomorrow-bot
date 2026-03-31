/**
 * @file governance_audit_layer.cpp
 * @brief Реализация слоя управления и аудита — runtime control plane
 */

#include "governance_audit_layer.hpp"

#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "health/health_service.hpp"
#include "persistence/event_journal.hpp"
#include "persistence/persistence_types.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace tb::governance {

// =============================================================================
// Конструктор
// =============================================================================

GovernanceAuditLayer::GovernanceAuditLayer(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<health::IHealthService> health,
    std::shared_ptr<persistence::EventJournal> journal)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , health_(std::move(health))
    , journal_(std::move(journal))
{
    // Инициализация кэшей метрик
    if (metrics_) {
        gauge_kill_switch_      = metrics_->gauge("governance_kill_switch_active");
        gauge_safe_mode_        = metrics_->gauge("governance_safe_mode_active");
        gauge_halt_mode_        = metrics_->gauge("governance_halt_mode");
        gauge_incident_state_   = metrics_->gauge("governance_incident_state");
        gauge_strategies_total_ = metrics_->gauge("governance_strategies_total");
        counter_audit_events_   = metrics_->counter("governance_audit_events_total");
    }

    // Регистрация в health-подсистеме при наличии
    if (health_) {
        register_with_health();
    }

    logger_->info("GovernanceAuditLayer", "Governance control plane инициализирован", {
        {"journal_enabled", journal_ ? "true" : "false"},
        {"metrics_enabled", metrics_ ? "true" : "false"},
        {"health_enabled",  health_ ? "true" : "false"}
    });
}

// =============================================================================
// Внутренний helper: текущая метка времени
// =============================================================================

Timestamp GovernanceAuditLayer::now() const {
    if (clock_) {
        return clock_->now();
    }
    return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// =============================================================================
// Реестр стратегий
// =============================================================================

VoidResult GovernanceAuditLayer::register_strategy(
    StrategyId id, StrategyVersion version, TradingMode mode) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (strategy_registry_.contains(id.get())) {
        logger_->warn("GovernanceAuditLayer", "Попытка повторной регистрации стратегии", {
            {"strategy_id", id.get()}
        });
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto ts = now();

    StrategyRegistryEntry entry;
    entry.strategy_id = id;
    entry.version = version;
    entry.enabled = true;
    entry.mode = mode;
    entry.registered_at = ts;
    entry.last_updated = ts;
    entry.config_hash = current_config_hash_;
    entry.lifecycle_state = StrategyLifecycleState::Registered;

    strategy_registry_[id.get()] = std::move(entry);

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyRegistered;
    record.timestamp = ts;
    record.actor = "system";
    record.target = "strategy:" + id.get();
    record.details = "Зарегистрирована версия " + std::to_string(version.get());
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    record.new_state = to_string(StrategyLifecycleState::Registered);
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Стратегия зарегистрирована", {
        {"strategy_id", id.get()},
        {"version", std::to_string(version.get())},
        {"mode", std::to_string(static_cast<int>(mode))}
    });

    update_metrics();
    return OkVoid();
}

VoidResult GovernanceAuditLayer::update_strategy_version(
    StrategyId id, StrategyVersion new_version) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto ts = now();
    auto old_version = it->second.version;
    it->second.version = new_version;
    it->second.last_updated = ts;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyVersionChanged;
    record.timestamp = ts;
    record.actor = "system";
    record.target = "strategy:" + id.get();
    record.details = "Версия обновлена: " + std::to_string(old_version.get()) +
                     " → " + std::to_string(new_version.get());
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    record.previous_state = std::to_string(old_version.get());
    record.new_state = std::to_string(new_version.get());
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Версия стратегии обновлена", {
        {"strategy_id", id.get()},
        {"old_version", std::to_string(old_version.get())},
        {"new_version", std::to_string(new_version.get())}
    });

    return OkVoid();
}

VoidResult GovernanceAuditLayer::enable_strategy(
    StrategyId id, const std::string& actor) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto ts = now();
    it->second.enabled = true;
    it->second.last_updated = ts;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyEnabled;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "strategy:" + id.get();
    record.details = "Стратегия включена";
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    record.previous_state = "disabled";
    record.new_state = "enabled";
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Стратегия включена", {
        {"strategy_id", id.get()},
        {"actor", actor}
    });

    update_metrics();
    return OkVoid();
}

VoidResult GovernanceAuditLayer::disable_strategy(
    StrategyId id, const std::string& actor) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto ts = now();
    it->second.enabled = false;
    it->second.last_updated = ts;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyDisabled;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "strategy:" + id.get();
    record.details = "Стратегия выключена";
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    record.previous_state = "enabled";
    record.new_state = "disabled";
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Стратегия выключена", {
        {"strategy_id", id.get()},
        {"actor", actor}
    });

    update_metrics();
    return OkVoid();
}

Result<StrategyRegistryEntry> GovernanceAuditLayer::get_strategy_info(
    const StrategyId& id) const {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return Err<StrategyRegistryEntry>(TbError::ConfigValidationFailed);
    }
    return Ok(StrategyRegistryEntry{it->second});
}

std::vector<StrategyRegistryEntry> GovernanceAuditLayer::get_all_strategies() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<StrategyRegistryEntry> result;
    result.reserve(strategy_registry_.size());
    for (const auto& [key, entry] : strategy_registry_) {
        result.push_back(entry);
    }
    return result;
}

// =============================================================================
// Жизненный цикл стратегии
// =============================================================================

VoidResult GovernanceAuditLayer::transition_strategy_lifecycle(
    StrategyId id, StrategyLifecycleState new_state, const std::string& actor) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        logger_->warn("GovernanceAuditLayer", "Переход lifecycle: стратегия не найдена", {
            {"strategy_id", id.get()}
        });
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto old_state = it->second.lifecycle_state;

    // Валидация допустимых переходов
    bool valid = false;
    switch (old_state) {
        case StrategyLifecycleState::Registered:
            valid = (new_state == StrategyLifecycleState::Shadow ||
                     new_state == StrategyLifecycleState::Disabled);
            break;
        case StrategyLifecycleState::Shadow:
            valid = (new_state == StrategyLifecycleState::Candidate ||
                     new_state == StrategyLifecycleState::Disabled ||
                     new_state == StrategyLifecycleState::Draining);
            break;
        case StrategyLifecycleState::Candidate:
            valid = (new_state == StrategyLifecycleState::Live ||
                     new_state == StrategyLifecycleState::Shadow ||
                     new_state == StrategyLifecycleState::Disabled);
            break;
        case StrategyLifecycleState::Live:
            valid = (new_state == StrategyLifecycleState::Draining ||
                     new_state == StrategyLifecycleState::Disabled);
            break;
        case StrategyLifecycleState::Draining:
            valid = (new_state == StrategyLifecycleState::Disabled ||
                     new_state == StrategyLifecycleState::Retired);
            break;
        case StrategyLifecycleState::Disabled:
            valid = (new_state == StrategyLifecycleState::Registered ||
                     new_state == StrategyLifecycleState::Retired);
            break;
        case StrategyLifecycleState::Retired:
            valid = false; // Терминальное состояние
            break;
    }

    if (!valid) {
        logger_->warn("GovernanceAuditLayer", "Недопустимый переход lifecycle", {
            {"strategy_id", id.get()},
            {"from", to_string(old_state)},
            {"to", to_string(new_state)}
        });
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto ts = now();
    it->second.lifecycle_state = new_state;
    it->second.last_updated = ts;

    // Определяем тип аудит-события для специфических переходов
    AuditEventType audit_type = AuditEventType::ModeTransition;
    if (new_state == StrategyLifecycleState::Draining) {
        audit_type = AuditEventType::StrategyDraining;
    } else if (new_state == StrategyLifecycleState::Retired) {
        audit_type = AuditEventType::StrategyRetired;
    }

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = audit_type;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "strategy:" + id.get();
    record.details = "Переход lifecycle: " + to_string(old_state) + " → " + to_string(new_state);
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    record.previous_state = to_string(old_state);
    record.new_state = to_string(new_state);
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Переход lifecycle стратегии", {
        {"strategy_id", id.get()},
        {"from", to_string(old_state)},
        {"to", to_string(new_state)},
        {"actor", actor}
    });

    update_metrics();
    return OkVoid();
}

bool GovernanceAuditLayer::is_strategy_allowed(const StrategyId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return false;
    }

    const auto& entry = it->second;
    if (!entry.enabled) {
        return false;
    }

    // Только Live и Shadow стратегии допущены к торговле
    return entry.lifecycle_state == StrategyLifecycleState::Live ||
           entry.lifecycle_state == StrategyLifecycleState::Shadow;
}

// =============================================================================
// Аудит
// =============================================================================

void GovernanceAuditLayer::record_audit(
    AuditEventType type, const std::string& actor,
    const std::string& target, const std::string& details,
    ConfigHash config_hash) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = type;
    record.timestamp = ts;
    record.actor = actor;
    record.target = target;
    record.details = details;
    record.config_hash = config_hash.get().empty() ? current_config_hash_ : config_hash;
    record.subsystem = "governance";
    record.severity = "info";

    audit_log_.push_back(record);
    persist_audit(record);

    // Ограничение размера in-memory лога
    if (audit_log_.size() > kMaxInMemoryAudit) {
        auto excess = audit_log_.size() - kMaxInMemoryAudit;
        audit_log_.erase(audit_log_.begin(),
                         audit_log_.begin() + static_cast<ptrdiff_t>(excess));
    }

    if (counter_audit_events_) {
        counter_audit_events_->increment();
    }
}

std::vector<AuditRecord> GovernanceAuditLayer::get_audit_log(size_t last_n) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (audit_log_.size() <= last_n) return audit_log_;

    return std::vector<AuditRecord>(
        audit_log_.end() - static_cast<ptrdiff_t>(last_n),
        audit_log_.end());
}

std::vector<AuditRecord> GovernanceAuditLayer::get_audit_log(
    Timestamp from, Timestamp to) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AuditRecord> result;
    for (const auto& record : audit_log_) {
        if (record.timestamp.get() >= from.get() && record.timestamp.get() <= to.get()) {
            result.push_back(record);
        }
    }
    return result;
}

// =============================================================================
// Governance состояние
// =============================================================================

void GovernanceAuditLayer::set_config_hash(ConfigHash hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_config_hash_ = std::move(hash);

    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::ConfigChanged;
    record.timestamp = ts;
    record.actor = "system";
    record.target = "config";
    record.details = "Хэш конфигурации обновлён: " + current_config_hash_.get();
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "info";
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Хэш конфигурации обновлён", {
        {"config_hash", current_config_hash_.get()}
    });
}

void GovernanceAuditLayer::set_runtime_version(std::string version) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_version_ = std::move(version);

    logger_->info("GovernanceAuditLayer", "Runtime версия установлена", {
        {"version", runtime_version_}
    });
}

void GovernanceAuditLayer::set_trading_mode(TradingMode mode, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto old_mode = current_mode_;
    current_mode_ = mode;
    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::ModeTransition;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "trading_mode";
    record.details = "Переход режима: " + std::to_string(static_cast<int>(old_mode)) +
                     " → " + std::to_string(static_cast<int>(mode));
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = "warn";
    record.previous_state = std::to_string(static_cast<int>(old_mode));
    record.new_state = std::to_string(static_cast<int>(mode));
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer", "Режим торговли изменён", {
        {"from", std::to_string(static_cast<int>(old_mode))},
        {"to", std::to_string(static_cast<int>(mode))},
        {"actor", actor}
    });

    update_metrics();
}

void GovernanceAuditLayer::set_kill_switch(bool active, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool was_active = kill_switch_active_;
    kill_switch_active_ = active;
    if (active) {
        kill_switch_reason_ = "Активирован оператором: " + actor;
    } else {
        kill_switch_reason_.clear();
    }

    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = active ? AuditEventType::KillSwitchActivated
                         : AuditEventType::KillSwitchDeactivated;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "kill_switch";
    record.details = active ? "Kill switch активирован" : "Kill switch деактивирован";
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = active ? "critical" : "warn";
    record.previous_state = was_active ? "active" : "inactive";
    record.new_state = active ? "active" : "inactive";
    audit_log_.push_back(record);
    persist_audit(record);

    if (active) {
        logger_->critical("GovernanceAuditLayer", "Kill switch АКТИВИРОВАН", {
            {"actor", actor}
        });
    } else {
        logger_->warn("GovernanceAuditLayer", "Kill switch деактивирован", {
            {"actor", actor}
        });
    }

    update_metrics();
    update_health_state();
}

void GovernanceAuditLayer::set_safe_mode(bool active, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool was_active = safe_mode_active_;
    safe_mode_active_ = active;
    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = active ? AuditEventType::SafeModeEntered
                         : AuditEventType::SafeModeExited;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "safe_mode";
    record.details = active ? "Вход в безопасный режим" : "Выход из безопасного режима";
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = active ? "warn" : "info";
    record.previous_state = was_active ? "active" : "inactive";
    record.new_state = active ? "active" : "inactive";
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->info("GovernanceAuditLayer",
                  active ? "Безопасный режим включён" : "Безопасный режим выключен", {
        {"actor", actor}
    });

    update_metrics();
    update_health_state();
}

GovernanceSnapshot GovernanceAuditLayer::get_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    GovernanceSnapshot snapshot;

    snapshot.strategy_registry.reserve(strategy_registry_.size());
    for (const auto& [key, entry] : strategy_registry_) {
        snapshot.strategy_registry.push_back(entry);
    }

    // Последние 50 записей аудита
    size_t audit_count = std::min(audit_log_.size(), size_t{50});
    if (audit_count > 0) {
        snapshot.recent_audit.assign(
            audit_log_.end() - static_cast<ptrdiff_t>(audit_count),
            audit_log_.end());
    }

    snapshot.current_config_hash = current_config_hash_;
    snapshot.runtime_version = runtime_version_;
    snapshot.current_mode = current_mode_;
    snapshot.kill_switch_active = kill_switch_active_;
    snapshot.safe_mode_active = safe_mode_active_;
    snapshot.snapshot_at = now();
    snapshot.halt_mode = halt_mode_;
    snapshot.incident_state = incident_state_;
    snapshot.incident_reason = incident_reason_;

    return snapshot;
}

// =============================================================================
// Режим остановки (Halt Mode)
// =============================================================================

void GovernanceAuditLayer::set_halt_mode(
    HaltMode mode, const std::string& actor, const std::string& reason) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto old_mode = halt_mode_;
    halt_mode_ = mode;
    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::HaltModeChanged;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "halt_mode";
    record.details = reason.empty()
        ? ("Переход halt mode: " + to_string(old_mode) + " → " + to_string(mode))
        : reason;
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = (mode == HaltMode::HardHalt) ? "critical" : "warn";
    record.previous_state = to_string(old_mode);
    record.new_state = to_string(mode);
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->warn("GovernanceAuditLayer", "Halt mode изменён", {
        {"from", to_string(old_mode)},
        {"to", to_string(mode)},
        {"actor", actor},
        {"reason", reason}
    });

    update_metrics();
    update_health_state();
}

HaltMode GovernanceAuditLayer::get_halt_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return halt_mode_;
}

// =============================================================================
// Инцидентная машина состояний
// =============================================================================

void GovernanceAuditLayer::transition_incident_state(
    IncidentState new_state, const std::string& actor, const std::string& reason) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto old_state = incident_state_;

    // Валидация переходов инцидентного FSM
    bool valid = false;
    switch (old_state) {
        case IncidentState::Normal:
            valid = (new_state == IncidentState::Degraded ||
                     new_state == IncidentState::Restricted ||
                     new_state == IncidentState::Halted);
            break;
        case IncidentState::Degraded:
            valid = (new_state == IncidentState::Normal ||
                     new_state == IncidentState::Restricted ||
                     new_state == IncidentState::Halted);
            break;
        case IncidentState::Restricted:
            valid = (new_state == IncidentState::Degraded ||
                     new_state == IncidentState::Halted ||
                     new_state == IncidentState::Recovering);
            break;
        case IncidentState::Halted:
            valid = (new_state == IncidentState::Recovering);
            break;
        case IncidentState::Recovering:
            valid = (new_state == IncidentState::Normal ||
                     new_state == IncidentState::Degraded ||
                     new_state == IncidentState::Halted);
            break;
    }

    if (!valid) {
        logger_->warn("GovernanceAuditLayer", "Недопустимый переход incident state", {
            {"from", to_string(old_state)},
            {"to", to_string(new_state)},
            {"actor", actor}
        });
        return;
    }

    incident_state_ = new_state;
    incident_reason_ = reason;
    auto ts = now();

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::IncidentStateChanged;
    record.timestamp = ts;
    record.actor = actor;
    record.target = "incident_state";
    record.details = reason;
    record.config_hash = current_config_hash_;
    record.subsystem = "governance";
    record.severity = (new_state == IncidentState::Halted) ? "critical" : "warn";
    record.previous_state = to_string(old_state);
    record.new_state = to_string(new_state);
    audit_log_.push_back(record);
    persist_audit(record);

    logger_->warn("GovernanceAuditLayer", "Incident state изменён", {
        {"from", to_string(old_state)},
        {"to", to_string(new_state)},
        {"actor", actor},
        {"reason", reason}
    });

    update_metrics();
    update_health_state();
}

IncidentState GovernanceAuditLayer::get_incident_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return incident_state_;
}

// =============================================================================
// Governance Gate
// =============================================================================

GovernanceGateResult GovernanceAuditLayer::evaluate_trading_gate(
    const StrategyId& strategy_id) const {

    std::lock_guard<std::mutex> lock(mutex_);

    GovernanceGateResult result;
    result.current_halt = halt_mode_;
    result.current_incident = incident_state_;

    // 1. Kill switch — полная блокировка
    if (kill_switch_active_) {
        result.trading_allowed = false;
        result.new_entries_allowed = false;
        result.close_only = false;
        result.denial_reason = "Kill switch активен: " + kill_switch_reason_;
        return result;
    }

    // 2. Halt mode
    switch (halt_mode_) {
        case HaltMode::HardHalt:
            result.trading_allowed = false;
            result.new_entries_allowed = false;
            result.close_only = false;
            result.denial_reason = "HardHalt: торговля полностью остановлена";
            return result;

        case HaltMode::CloseOnly:
            result.trading_allowed = true;
            result.new_entries_allowed = false;
            result.close_only = true;
            break;

        case HaltMode::ReduceOnly:
            result.trading_allowed = true;
            result.new_entries_allowed = false;
            result.close_only = false;
            break;

        case HaltMode::NoNewEntries:
            result.trading_allowed = true;
            result.new_entries_allowed = false;
            result.close_only = false;
            break;

        case HaltMode::None:
            result.trading_allowed = true;
            result.new_entries_allowed = true;
            result.close_only = false;
            break;
    }

    // 3. Incident state
    switch (incident_state_) {
        case IncidentState::Halted:
            result.trading_allowed = false;
            result.new_entries_allowed = false;
            result.close_only = false;
            result.denial_reason = "Incident state Halted: " + incident_reason_;
            return result;

        case IncidentState::Restricted:
            result.new_entries_allowed = false;
            result.close_only = true;
            break;

        case IncidentState::Degraded:
            result.new_entries_allowed = false;
            break;

        case IncidentState::Recovering:
            result.new_entries_allowed = false;
            break;

        case IncidentState::Normal:
            break;
    }

    // 4. Safe mode — запрет новых позиций
    if (safe_mode_active_) {
        result.new_entries_allowed = false;
        if (result.denial_reason.empty()) {
            result.denial_reason = "Safe mode активен";
        }
    }

    // 5. Проверка стратегии (пропускаем для глобальной проверки с пустым ID)
    if (strategy_id.get().empty()) {
        return result;
    }

    auto it = strategy_registry_.find(strategy_id.get());
    if (it == strategy_registry_.end()) {
        result.trading_allowed = false;
        result.new_entries_allowed = false;
        result.denial_reason = "Стратегия не зарегистрирована: " + strategy_id.get();
        return result;
    }

    const auto& entry = it->second;
    if (!entry.enabled) {
        result.trading_allowed = false;
        result.new_entries_allowed = false;
        result.denial_reason = "Стратегия выключена: " + strategy_id.get();
        return result;
    }

    // 6. Lifecycle state — только Live и Shadow допущены
    if (entry.lifecycle_state != StrategyLifecycleState::Live &&
        entry.lifecycle_state != StrategyLifecycleState::Shadow) {
        result.trading_allowed = false;
        result.new_entries_allowed = false;
        result.denial_reason = "Стратегия в состоянии " +
            to_string(entry.lifecycle_state) + ", торговля не разрешена";
        return result;
    }

    return result;
}

// =============================================================================
// Health
// =============================================================================

void GovernanceAuditLayer::register_with_health() {
    if (!health_) return;

    health_->register_subsystem("governance");
    health_->update_subsystem("governance", health::SubsystemState::Healthy,
                              "Governance control plane активен");

    logger_->info("GovernanceAuditLayer", "Зарегистрирован в health-подсистеме");
}

// =============================================================================
// Kill switch запросы
// =============================================================================

bool GovernanceAuditLayer::is_kill_switch_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kill_switch_active_;
}

std::string GovernanceAuditLayer::get_kill_switch_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kill_switch_reason_;
}

// =============================================================================
// Внутренние методы
// =============================================================================

void GovernanceAuditLayer::persist_audit(const AuditRecord& record) {
    if (!journal_) return;

    try {
        // Сериализация в JSON вручную (без зависимости от boost::json)
        std::string json = "{";
        json += "\"audit_id\":" + std::to_string(record.audit_id) + ",";
        json += "\"type\":\"" + to_string(record.type) + "\",";
        json += "\"timestamp\":" + std::to_string(record.timestamp.get()) + ",";
        json += "\"actor\":\"" + record.actor + "\",";
        json += "\"target\":\"" + record.target + "\",";
        json += "\"details\":\"" + record.details + "\",";
        json += "\"config_hash\":\"" + record.config_hash.get() + "\",";
        json += "\"subsystem\":\"" + record.subsystem + "\",";
        json += "\"severity\":\"" + record.severity + "\"";
        if (!record.previous_state.empty()) {
            json += ",\"previous_state\":\"" + record.previous_state + "\"";
        }
        if (!record.new_state.empty()) {
            json += ",\"new_state\":\"" + record.new_state + "\"";
        }
        if (!record.metadata_json.empty()) {
            json += ",\"metadata\":" + record.metadata_json;
        }
        json += "}";

        auto result = journal_->append(
            persistence::JournalEntryType::GovernanceEvent,
            json,
            CorrelationId{""},
            StrategyId{""},
            record.config_hash
        );

        if (!result.has_value()) {
            logger_->warn("GovernanceAuditLayer", "Не удалось персистировать аудит-запись", {
                {"audit_id", std::to_string(record.audit_id)},
                {"type", to_string(record.type)}
            });
        }
    } catch (const std::exception& e) {
        logger_->warn("GovernanceAuditLayer", "Исключение при персистировании аудита", {
            {"audit_id", std::to_string(record.audit_id)},
            {"error", e.what()}
        });
    } catch (...) {
        logger_->warn("GovernanceAuditLayer", "Неизвестное исключение при персистировании аудита", {
            {"audit_id", std::to_string(record.audit_id)}
        });
    }
}

void GovernanceAuditLayer::update_metrics() {
    if (!metrics_) return;

    if (gauge_kill_switch_) {
        gauge_kill_switch_->set(kill_switch_active_ ? 1.0 : 0.0);
    }
    if (gauge_safe_mode_) {
        gauge_safe_mode_->set(safe_mode_active_ ? 1.0 : 0.0);
    }
    if (gauge_halt_mode_) {
        gauge_halt_mode_->set(static_cast<double>(halt_mode_));
    }
    if (gauge_incident_state_) {
        gauge_incident_state_->set(static_cast<double>(incident_state_));
    }
    if (gauge_strategies_total_) {
        gauge_strategies_total_->set(static_cast<double>(strategy_registry_.size()));
    }
}

void GovernanceAuditLayer::update_health_state() {
    if (!health_) return;

    // Маппинг governance состояния на health state
    if (kill_switch_active_ || incident_state_ == IncidentState::Halted) {
        health_->update_subsystem("governance", health::SubsystemState::Failed,
                                  "Kill switch или Halted состояние");
    } else if (incident_state_ == IncidentState::Degraded ||
               incident_state_ == IncidentState::Restricted ||
               incident_state_ == IncidentState::Recovering ||
               halt_mode_ != HaltMode::None || safe_mode_active_) {
        health_->update_subsystem("governance", health::SubsystemState::Degraded,
                                  "Ограниченный режим работы: halt=" + to_string(halt_mode_) +
                                  " incident=" + to_string(incident_state_));
    } else {
        health_->update_subsystem("governance", health::SubsystemState::Healthy,
                                  "Governance control plane работает штатно");
    }
}

} // namespace tb::governance
