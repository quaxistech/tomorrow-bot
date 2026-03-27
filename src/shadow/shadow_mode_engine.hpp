#pragma once
/**
 * @file shadow_mode_engine.hpp
 * @brief Ядро shadow trading подсистемы — professional-grade виртуальное исполнение
 *
 * Записывает теневые решения, симулирует исполнение с учётом комиссий,
 * отслеживает цены в настраиваемых окнах, считает gross/net P&L,
 * ведёт shadow-позиции, формирует сравнение с live, генерирует алерты,
 * экспортирует метрики.
 *
 * КРИТИЧЕСКИ ВАЖНО: никогда не порождает реальных ордеров.
 */
#include "shadow/shadow_types.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "governance/governance_audit_layer.hpp"
#include "persistence/storage_adapter.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::shadow {

/**
 * @class ShadowModeEngine
 * @brief Движок теневого режима для спотовой крипто-торговли
 *
 * Потокобезопасный. Все публичные методы защищены мьютексом.
 * Опциональные зависимости (metrics, governance, storage) принимаются
 * как nullptr и безопасно пропускаются, если не предоставлены.
 */
class ShadowModeEngine {
public:
    ShadowModeEngine(
        ShadowConfig config,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr,
        std::shared_ptr<governance::GovernanceAuditLayer> governance = nullptr,
        std::shared_ptr<persistence::IStorageAdapter> storage = nullptr);

    // ======================== Core operations ========================

    /// Записать теневое решение и симулировать исполнение
    [[nodiscard]] VoidResult record_decision(ShadowDecision decision);

    /// Обновить отслеживание цен для всех незавершённых записей
    void update_price_tracking(Symbol symbol, Price current_price, Timestamp now);

    /// Очистить зависшие/просроченные записи
    void cleanup_stale_records(Timestamp now);

    // ======================== Queries ================================

    /// Все записи по стратегии
    [[nodiscard]] std::vector<ShadowTradeRecord> get_trades(StrategyId strategy_id) const;

    /// Количество записей по стратегии
    [[nodiscard]] std::size_t get_trade_count(StrategyId strategy_id) const;

    /// Текущие shadow-позиции
    [[nodiscard]] std::vector<ShadowPosition> get_positions() const;

    // ======================== Analytics ==============================

    /// Сравнение shadow vs live
    [[nodiscard]] ShadowComparison compare(
        StrategyId strategy_id,
        int live_trades, double live_pnl_bps, double live_hit_rate) const;

    /// Агрегированные метрики
    [[nodiscard]] ShadowMetricsSummary get_metrics_summary() const;

    /// Генерировать алерты при дивергенции
    [[nodiscard]] std::vector<ShadowAlert> check_alerts(
        StrategyId strategy_id,
        int live_trades, double live_pnl_bps, double live_hit_rate) const;

    // ======================== Control ================================

    [[nodiscard]] bool is_enabled() const;
    void set_enabled(bool enabled);
    void set_kill_switch(bool active);

private:
    ShadowConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<governance::GovernanceAuditLayer> governance_;
    std::shared_ptr<persistence::IStorageAdapter> storage_;

    mutable std::mutex mutex_;
    bool kill_switch_active_{false};

    /// Записи по strategy_id
    std::unordered_map<std::string, std::deque<ShadowTradeRecord>> records_;

    /// Shadow-позиции по ключу "strategy_id:symbol"
    std::unordered_map<std::string, ShadowPosition> positions_;

    // ======================== Private helpers ========================

    ShadowFillSimulation simulate_fill(const ShadowDecision& decision) const;
    double compute_gross_pnl_bps(const ShadowTradeRecord& record) const;
    double compute_net_pnl_bps(double gross_bps, const ShadowFillSimulation& fill) const;
    void update_position(const ShadowTradeRecord& record);
    void evict_oldest(std::deque<ShadowTradeRecord>& deque);
    void export_metrics(const ShadowTradeRecord& record);
    void persist_record(const ShadowTradeRecord& record);
    std::string make_position_key(const StrategyId& sid, const Symbol& sym) const;
};

} // namespace tb::shadow
