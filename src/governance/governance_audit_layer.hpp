/**
 * @file governance_audit_layer.hpp
 * @brief Слой управления и аудита
 *
 * Обеспечивает полный аудит всех торговых решений,
 * реестр стратегий и управление governance состоянием.
 */
#pragma once

#include "governance_types.hpp"
#include "common/types.hpp"
#include "common/result.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::governance {

/// Слой управления и аудита торговой системы
class GovernanceAuditLayer {
public:
    GovernanceAuditLayer();

    // === Реестр стратегий ===

    /// Зарегистрировать стратегию в реестре
    VoidResult register_strategy(StrategyId id, StrategyVersion version, TradingMode mode);

    /// Обновить версию стратегии
    VoidResult update_strategy_version(StrategyId id, StrategyVersion new_version);

    /// Включить стратегию
    VoidResult enable_strategy(StrategyId id, const std::string& actor);

    /// Выключить стратегию
    VoidResult disable_strategy(StrategyId id, const std::string& actor);

    /// Получить информацию о стратегии
    Result<StrategyRegistryEntry> get_strategy_info(const StrategyId& id) const;

    /// Получить все зарегистрированные стратегии
    std::vector<StrategyRegistryEntry> get_all_strategies() const;

    // === Аудит ===

    /// Записать аудиторское событие
    void record_audit(AuditEventType type, const std::string& actor,
                      const std::string& target, const std::string& details,
                      ConfigHash config_hash = ConfigHash(""));

    /// Получить последние N записей аудита
    std::vector<AuditRecord> get_audit_log(size_t last_n = 100) const;

    /// Получить записи аудита за период
    std::vector<AuditRecord> get_audit_log(Timestamp from, Timestamp to) const;

    // === Governance состояние ===

    /// Установить хэш текущей конфигурации
    void set_config_hash(ConfigHash hash);

    /// Установить версию runtime
    void set_runtime_version(std::string version);

    /// Установить режим торговли
    void set_trading_mode(TradingMode mode, const std::string& actor);

    /// Установить состояние kill switch
    void set_kill_switch(bool active, const std::string& actor);

    /// Установить безопасный режим
    void set_safe_mode(bool active, const std::string& actor);

    /// Получить снимок governance состояния
    GovernanceSnapshot get_snapshot() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StrategyRegistryEntry> strategy_registry_;
    std::vector<AuditRecord> audit_log_;
    std::atomic<uint64_t> audit_counter_{0};
    ConfigHash current_config_hash_{""};
    std::string runtime_version_;
    TradingMode current_mode_{TradingMode::Paper};
    bool kill_switch_active_{false};
    bool safe_mode_active_{false};
};

} // namespace tb::governance
