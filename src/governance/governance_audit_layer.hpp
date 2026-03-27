/**
 * @file governance_audit_layer.hpp
 * @brief Слой управления и аудита — runtime control plane торговой системы
 *
 * Обеспечивает полный аудит всех торговых решений,
 * реестр стратегий, governance gate для пайплайна,
 * управление режимами остановки (HaltMode),
 * инцидентную машину состояний (IncidentState),
 * жизненный цикл стратегий, дурабельный аудит через EventJournal,
 * регистрацию в health-подсистеме и экспорт метрик.
 */
#pragma once

#include "governance_types.hpp"
#include "common/types.hpp"
#include "common/result.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::logging { class ILogger; }
namespace tb::clock { class IClock; }
namespace tb::metrics { class IMetricsRegistry; class IGauge; class ICounter; }
namespace tb::health { class IHealthService; }
namespace tb::persistence { class EventJournal; }

namespace tb::governance {

/// Слой управления и аудита торговой системы — runtime control plane
class GovernanceAuditLayer {
public:
    /**
     * @brief Конструктор с зависимостями
     * @param logger     Логгер (обязателен)
     * @param clock      Часы для генерации меток времени (обязательны)
     * @param metrics    Реестр метрик (опционален)
     * @param health     Сервис здоровья (опционален)
     * @param journal    Журнал событий для дурабельного аудита (опционален)
     */
    GovernanceAuditLayer(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr,
        std::shared_ptr<health::IHealthService> health = nullptr,
        std::shared_ptr<persistence::EventJournal> journal = nullptr
    );

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

    // === Жизненный цикл стратегии ===

    /// Переход жизненного цикла стратегии (с валидацией и аудитом)
    VoidResult transition_strategy_lifecycle(StrategyId id,
                                             StrategyLifecycleState new_state,
                                             const std::string& actor);

    /// Проверить, разрешена ли стратегия для торговли (enabled + Live/Shadow)
    [[nodiscard]] bool is_strategy_allowed(const StrategyId& id) const;

    // === Аудит ===

    /// Записать аудиторское событие (в in-memory лог + persistent journal)
    void record_audit(AuditEventType type, const std::string& actor,
                      const std::string& target, const std::string& details,
                      ConfigHash config_hash = ConfigHash(""));

    /// Получить последние N записей аудита
    [[nodiscard]] std::vector<AuditRecord> get_audit_log(size_t last_n = 100) const;

    /// Получить записи аудита за период
    [[nodiscard]] std::vector<AuditRecord> get_audit_log(Timestamp from, Timestamp to) const;

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
    [[nodiscard]] GovernanceSnapshot get_snapshot() const;

    // === Режим остановки (Halt Mode) ===

    /// Установить режим остановки торговли
    void set_halt_mode(HaltMode mode, const std::string& actor,
                       const std::string& reason = "");

    /// Получить текущий режим остановки
    [[nodiscard]] HaltMode get_halt_mode() const;

    // === Инцидентная машина состояний ===

    /// Переход инцидентного состояния (с валидацией переходов и аудитом)
    void transition_incident_state(IncidentState new_state,
                                   const std::string& actor,
                                   const std::string& reason);

    /// Получить текущее инцидентное состояние
    [[nodiscard]] IncidentState get_incident_state() const;

    // === Governance Gate (вызывается пайплайном перед торговлей) ===

    /// Оценить, разрешена ли торговля для стратегии
    [[nodiscard]] GovernanceGateResult evaluate_trading_gate(
        const StrategyId& strategy_id) const;

    // === Регистрация в health-подсистеме ===

    /// Зарегистрировать governance как подсистему здоровья
    void register_with_health();

    // === Kill switch запросы (делегирование от risk engine) ===

    /// Проверить, активен ли kill switch
    [[nodiscard]] bool is_kill_switch_active() const;

    /// Получить причину активации kill switch
    [[nodiscard]] std::string get_kill_switch_reason() const;

private:
    // --- Зависимости ---
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<health::IHealthService> health_;
    std::shared_ptr<persistence::EventJournal> journal_;

    // --- Состояние ---
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StrategyRegistryEntry> strategy_registry_;
    std::vector<AuditRecord> audit_log_;
    std::atomic<uint64_t> audit_counter_{0};
    ConfigHash current_config_hash_{""};
    std::string runtime_version_;
    TradingMode current_mode_{TradingMode::Paper};
    bool kill_switch_active_{false};
    std::string kill_switch_reason_;
    bool safe_mode_active_{false};
    HaltMode halt_mode_{HaltMode::None};
    IncidentState incident_state_{IncidentState::Normal};
    std::string incident_reason_;

    /// Максимальный размер in-memory аудит-лога
    static constexpr size_t kMaxInMemoryAudit = 10000;

    // --- Кэши метрик ---
    std::shared_ptr<metrics::IGauge> gauge_kill_switch_;
    std::shared_ptr<metrics::IGauge> gauge_safe_mode_;
    std::shared_ptr<metrics::IGauge> gauge_halt_mode_;
    std::shared_ptr<metrics::IGauge> gauge_incident_state_;
    std::shared_ptr<metrics::IGauge> gauge_strategies_total_;
    std::shared_ptr<metrics::ICounter> counter_audit_events_;

    // --- Внутренние методы ---

    /// Персистировать аудит-запись в EventJournal (best-effort)
    void persist_audit(const AuditRecord& record);

    /// Обновить метрики governance
    void update_metrics();

    /// Обновить состояние здоровья подсистемы
    void update_health_state();

    /// Получить текущую метку времени (clock_ → fallback steady_clock)
    [[nodiscard]] Timestamp now() const;
};

} // namespace tb::governance
