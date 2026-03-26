#pragma once
/**
 * @file alpha_decay_monitor.hpp
 * @brief Монитор угасания альфы — отслеживание деградации стратегий
 *
 * Анализирует скользящие окна результатов сделок и вычисляет
 * метрики деградации по 7 измерениям:
 *   1. Expectancy          — средняя ожидаемая доходность
 *   2. HitRate             — процент прибыльных сделок
 *   3. SlippageAdjusted    — деградация с учётом проскальзывания
 *   4. RegimeConditioned   — раздельный анализ по режимам рынка
 *   5. ConfidenceReliab.   — калибровка убеждённости (Brier score)
 *   6. ExecutionQuality    — чистый P&L после транзакционных издержек
 *   7. AdverseExcursion    — среднее максимальное неблагоприятное отклонение
 *
 * Интегрирован с MetricsRegistry (Prometheus-gauge на здоровье) и
 * IStorageAdapter (персистентность истории сделок через PostgreSQL).
 */
#include "alpha_decay/alpha_decay_types.hpp"
#include "common/result.hpp"
#include "metrics/metrics_registry.hpp"
#include "metrics/gauge.hpp"
#include "metrics/counter.hpp"
#include "persistence/storage_adapter.hpp"
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>
#include <memory>

namespace tb::alpha_decay {

/// Монитор угасания альфы стратегий
class AlphaDecayMonitor {
public:
    /// Конфигурация + опциональные интеграции
    explicit AlphaDecayMonitor(
        DecayConfig config = {},
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr,
        std::shared_ptr<persistence::IStorageAdapter> storage = nullptr);

    /// Записать результат сделки для стратегии
    void record_trade_outcome(const StrategyId& strategy_id, const TradeOutcome& outcome);

    /// Анализировать деградацию стратегии
    Result<AlphaDecayReport> analyze(const StrategyId& strategy_id);

    /// Проверка — деградирована ли стратегия
    [[nodiscard]] bool is_degraded(const StrategyId& strategy_id);

    /// Получить отчёты по всем стратегиям
    [[nodiscard]] std::vector<AlphaDecayReport> get_all_reports();

    /// Загрузить историю сделок из хранилища (вызывать при старте)
    void restore_from_storage();

private:
    /// Состояние гистерезиса рекомендации на одну стратегию
    struct HysteresisState {
        DecayRecommendation last_confirmed{DecayRecommendation::NoAction};
        DecayRecommendation pending{DecayRecommendation::NoAction};
        int stable_count{0};
    };

    // === Вычисление метрик ===

    DecayMetric compute_expectancy(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_hit_rate(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_slippage(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_regime_conditioned(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_confidence_reliability(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_execution_quality(const std::deque<TradeOutcome>& trades) const;
    DecayMetric compute_adverse_excursion(const std::deque<TradeOutcome>& trades) const;

    /// Определить рекомендацию с гистерезисом
    DecayRecommendation determine_recommendation(
        double health, const std::string& strategy_key);

    /// Генерировать алерты из метрик
    std::vector<DecayAlert> generate_alerts(
        const StrategyId& strategy_id,
        const std::vector<DecayMetric>& metrics) const;

    /// Вычислить здоровье из метрик (направленная формула)
    double compute_health(const std::vector<DecayMetric>& metrics) const;

    /// Внутренний analyze без захвата мьютекса (вызывается под mutex_)
    AlphaDecayReport analyze_unlocked(
        const std::string& key, const std::deque<TradeOutcome>& trades);

    /// Экспорт метрик здоровья в MetricsRegistry
    void export_metrics(const AlphaDecayReport& report);

    /// Персистировать результат сделки в storage
    void persist_trade(const StrategyId& strategy_id, const TradeOutcome& outcome);

    DecayConfig config_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<persistence::IStorageAdapter> storage_;

    std::unordered_map<std::string, std::deque<TradeOutcome>> trade_history_;
    std::unordered_map<std::string, HysteresisState> hysteresis_;
    mutable std::mutex mutex_;
};

} // namespace tb::alpha_decay

