#pragma once
/**
 * @file alpha_decay_monitor.hpp
 * @brief Монитор угасания альфы — отслеживание деградации стратегий
 *
 * Анализирует скользящие окна результатов сделок и вычисляет
 * метрики деградации по нескольким измерениям.
 */
#include "alpha_decay/alpha_decay_types.hpp"
#include "common/result.hpp"
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>

namespace tb::alpha_decay {

/// Монитор угасания альфы стратегий
class AlphaDecayMonitor {
public:
    /// Конструктор принимает конфигурацию
    explicit AlphaDecayMonitor(DecayConfig config = {});

    /// Записать результат сделки для стратегии
    void record_trade_outcome(const StrategyId& strategy_id, const TradeOutcome& outcome);

    /// Анализировать деградацию стратегии
    Result<AlphaDecayReport> analyze(const StrategyId& strategy_id);

    /// Проверка — деградирована ли стратегия
    [[nodiscard]] bool is_degraded(const StrategyId& strategy_id);

    /// Получить отчёты по всем стратегиям
    [[nodiscard]] std::vector<AlphaDecayReport> get_all_reports();

private:
    /// Вычислить метрику ожидаемой доходности
    DecayMetric compute_expectancy(const std::deque<TradeOutcome>& trades) const;

    /// Вычислить метрику hit rate
    DecayMetric compute_hit_rate(const std::deque<TradeOutcome>& trades) const;

    /// Вычислить метрику проскальзывания
    DecayMetric compute_slippage(const std::deque<TradeOutcome>& trades) const;

    /// Определить рекомендацию по здоровью
    DecayRecommendation determine_recommendation(double health) const;

    /// Генерировать алерты из метрик
    std::vector<DecayAlert> generate_alerts(
        const StrategyId& strategy_id,
        const std::vector<DecayMetric>& metrics) const;

    DecayConfig config_;
    std::unordered_map<std::string, std::deque<TradeOutcome>> trade_history_;
    mutable std::mutex mutex_;
};

} // namespace tb::alpha_decay
