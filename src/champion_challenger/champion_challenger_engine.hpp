#pragma once
/**
 * @file champion_challenger_engine.hpp
 * @brief Движок Champion-Challenger A/B тестирования стратегий (v2)
 *
 * Ключевые улучшения vs v1:
 *  - Принимает PnLBreakdown: чистый P&L после комиссий и проскальзывания
 *  - Pre-promotion audit: многокритериальная проверка перед promote()
 *  - Трекинг drawdown и hit rate для каждой стороны
 *  - Per-regime статистика используется в аудите consistency
 *  - Экспорт метрик в MetricsRegistry (Prometheus)
 *  - Персистентность через IStorageAdapter (audit trail)
 *  - Observer pattern: callbacks при promote/reject
 *  - Структурированное логирование через ILogger
 */
#include "champion_challenger/champion_challenger_types.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "persistence/storage_adapter.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>

namespace tb::champion_challenger {

class ChampionChallengerEngine {
public:
    /**
     * @param config   Пороги и параметры оценки
     * @param logger   Структурированный логгер (nullptr = молчание)
     * @param metrics  Реестр метрик Prometheus (nullptr = нет экспорта)
     * @param storage  Адаптер хранилища для audit trail (nullptr = in-memory)
     */
    explicit ChampionChallengerEngine(
        ChampionChallengerConfig config,
        std::shared_ptr<logging::ILogger>             logger  = nullptr,
        std::shared_ptr<metrics::IMetricsRegistry>    metrics = nullptr,
        std::shared_ptr<persistence::IStorageAdapter> storage = nullptr);

    // --- Регистрация ---

    /// Зарегистрировать challenger для champion-стратегии
    [[nodiscard]] VoidResult register_challenger(
        StrategyId champion,
        StrategyId challenger,
        StrategyVersion version);

    // --- Запись результатов ---

    /// Записать результат champion (чистый P&L после комиссий и проскальзывания)
    [[nodiscard]] VoidResult record_champion_outcome(
        StrategyId         champion,
        PnLBreakdown       breakdown,
        const std::string& regime);

    /// Записать результат challenger (чистый P&L после комиссий и проскальзывания)
    [[nodiscard]] VoidResult record_challenger_outcome(
        StrategyId         challenger,
        PnLBreakdown       breakdown,
        const std::string& regime,
        double             conviction);

    // --- Оценка ---

    /// Сформировать отчёт сравнения для champion-стратегии
    [[nodiscard]] Result<ChampionChallengerReport> evaluate(StrategyId champion) const;

    /// Провести pre-promotion аудит без изменения статуса
    [[nodiscard]] PrePromotionAudit audit_challenger(StrategyId challenger) const;

    /// Проверить, прошёл ли challenger все критерии для promote
    [[nodiscard]] bool should_promote(StrategyId challenger) const;

    /// Проверить, следует ли отклонить challenger
    [[nodiscard]] bool should_reject(StrategyId challenger) const;

    // --- Действия ---

    /**
     * @brief Повысить challenger (с pre-promotion аудитом)
     * @param force Если true — пропустить аудит (для ручного вмешательства)
     */
    [[nodiscard]] VoidResult promote(StrategyId challenger, bool force = false);

    /// Отклонить challenger
    [[nodiscard]] VoidResult reject(StrategyId challenger);

    // --- Observer ---

    /// Добавить наблюдателя (MetricsExporter, AlertManager, etc.)
    void add_observer(std::shared_ptr<IChallengerObserver> observer);

private:
    ChampionChallengerConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ChallengerEntry> entries_;

    std::shared_ptr<logging::ILogger>             logger_;
    std::shared_ptr<metrics::IMetricsRegistry>    metrics_;
    std::shared_ptr<persistence::IStorageAdapter> storage_;
    std::vector<std::shared_ptr<IChallengerObserver>> observers_;

    /// Чистый relative P&L delta: (challenger_net - champion_net) / |champion_net|
    [[nodiscard]] double compute_performance_delta(const ChallengerEntry& entry) const;

    /// Обновить peak/drawdown после новой сделки
    void update_drawdown(ComparisonMetrics& m) const;

    /// Запустить полный pre-promotion аудит (вызывается под mutex_)
    [[nodiscard]] PrePromotionAudit run_pre_promotion_audit(const ChallengerEntry& entry) const;

    /// Экспортировать gauge-метрики для одного челленджера
    void export_metrics(const ChallengerEntry& entry) const;

    /// Записать событие в journal хранилища
    void persist_event(const StrategyId& challenger_id, const std::string& event_type,
                       const std::string& payload_json) const;

    /// Уведомить наблюдателей о промоушене (вызывается вне mutex_)
    void notify_promotion(const ChallengerEntry& entry,
                          const std::vector<std::shared_ptr<IChallengerObserver>>& observers);

    /// Уведомить наблюдателей об отклонении (вызывается вне mutex_)
    void notify_rejection(const ChallengerEntry& entry,
                          const std::vector<std::shared_ptr<IChallengerObserver>>& observers);
};

} // namespace tb::champion_challenger
