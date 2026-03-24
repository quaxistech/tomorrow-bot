/**
 * @file governance_audit_layer.cpp
 * @brief Реализация слоя управления и аудита
 */

#include "governance_audit_layer.hpp"

#include <algorithm>
#include <chrono>

namespace tb::governance {

GovernanceAuditLayer::GovernanceAuditLayer() = default;

// === Реестр стратегий ===

VoidResult GovernanceAuditLayer::register_strategy(
    StrategyId id, StrategyVersion version, TradingMode mode) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Проверяем, не зарегистрирована ли уже
    if (strategy_registry_.contains(id.get())) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    StrategyRegistryEntry entry;
    entry.strategy_id = id;
    entry.version = version;
    entry.enabled = true;
    entry.mode = mode;
    entry.registered_at = now;
    entry.last_updated = now;
    entry.config_hash = current_config_hash_;

    strategy_registry_[id.get()] = std::move(entry);

    // Запись в аудит (без блокировки — уже внутри lock)
    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyRegistered;
    record.timestamp = now;
    record.actor = "system";
    record.target = "strategy:" + id.get();
    record.details = "Зарегистрирована версия " + std::to_string(version.get());
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));

    return OkVoid();
}

VoidResult GovernanceAuditLayer::update_strategy_version(
    StrategyId id, StrategyVersion new_version) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    auto old_version = it->second.version;
    it->second.version = new_version;
    it->second.last_updated = now;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyVersionChanged;
    record.timestamp = now;
    record.actor = "system";
    record.target = "strategy:" + id.get();
    record.details = "Версия обновлена: " + std::to_string(old_version.get()) +
                     " → " + std::to_string(new_version.get());
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));

    return OkVoid();
}

VoidResult GovernanceAuditLayer::enable_strategy(
    StrategyId id, const std::string& actor) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    it->second.enabled = true;
    it->second.last_updated = now;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyEnabled;
    record.timestamp = now;
    record.actor = actor;
    record.target = "strategy:" + id.get();
    record.details = "Стратегия включена";
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));

    return OkVoid();
}

VoidResult GovernanceAuditLayer::disable_strategy(
    StrategyId id, const std::string& actor) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategy_registry_.find(id.get());
    if (it == strategy_registry_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    it->second.enabled = false;
    it->second.last_updated = now;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::StrategyDisabled;
    record.timestamp = now;
    record.actor = actor;
    record.target = "strategy:" + id.get();
    record.details = "Стратегия выключена";
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));

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

// === Аудит ===

void GovernanceAuditLayer::record_audit(
    AuditEventType type, const std::string& actor,
    const std::string& target, const std::string& details,
    ConfigHash config_hash) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = type;
    record.timestamp = now;
    record.actor = actor;
    record.target = target;
    record.details = details;
    record.config_hash = config_hash.get().empty() ? current_config_hash_ : config_hash;
    audit_log_.push_back(std::move(record));
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

// === Governance состояние ===

void GovernanceAuditLayer::set_config_hash(ConfigHash hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_config_hash_ = std::move(hash);

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::ConfigChanged;
    record.timestamp = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    record.actor = "system";
    record.target = "config";
    record.details = "Хэш конфигурации обновлён: " + current_config_hash_.get();
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));
}

void GovernanceAuditLayer::set_runtime_version(std::string version) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_version_ = std::move(version);
}

void GovernanceAuditLayer::set_trading_mode(TradingMode mode, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto old_mode = current_mode_;
    current_mode_ = mode;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = AuditEventType::ModeTransition;
    record.timestamp = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    record.actor = actor;
    record.target = "trading_mode";
    record.details = "Переход режима: " + std::to_string(static_cast<int>(old_mode)) +
                     " → " + std::to_string(static_cast<int>(mode));
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));
}

void GovernanceAuditLayer::set_kill_switch(bool active, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    kill_switch_active_ = active;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = active ? AuditEventType::KillSwitchActivated
                         : AuditEventType::KillSwitchDeactivated;
    record.timestamp = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    record.actor = actor;
    record.target = "kill_switch";
    record.details = active ? "Kill switch активирован" : "Kill switch деактивирован";
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));
}

void GovernanceAuditLayer::set_safe_mode(bool active, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);

    safe_mode_active_ = active;

    AuditRecord record;
    record.audit_id = audit_counter_.fetch_add(1);
    record.type = active ? AuditEventType::SafeModeEntered
                         : AuditEventType::SafeModeExited;
    record.timestamp = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    record.actor = actor;
    record.target = "safe_mode";
    record.details = active ? "Вход в безопасный режим" : "Выход из безопасного режима";
    record.config_hash = current_config_hash_;
    audit_log_.push_back(std::move(record));
}

GovernanceSnapshot GovernanceAuditLayer::get_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    GovernanceSnapshot snapshot;

    // Стратегии
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
    snapshot.snapshot_at = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return snapshot;
}

} // namespace tb::governance
