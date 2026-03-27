#pragma once
#include "opportunity_cost/opportunity_cost_types.hpp"
#include "strategy/strategy_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>

namespace tb::opportunity_cost {

/// Интерфейс движка стоимости упущенных возможностей
class IOpportunityCostEngine {
public:
    virtual ~IOpportunityCostEngine() = default;

    /// Оценить стоимость возможности для данного намерения
    virtual OpportunityCostResult evaluate(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const PortfolioContext& portfolio_ctx,
        double conviction_threshold
    ) = 0;
};

/// Конфигурация модуля opportunity cost (загружается из YAML)
struct OpportunityCostConfig {
    // ── Пороги net edge (базисные пункты) ──
    double min_net_expected_bps{0.0};          ///< Мин чистый ожидаемый доход для входа
    double execute_min_net_bps{15.0};          ///< Мин чистый доход для немедленного исполнения

    // ── Пороги экспозиции ──
    double high_exposure_threshold{0.75};      ///< Порог высокой экспозиции [0,1]
    double high_exposure_min_conviction{0.65}; ///< Мин conviction при высокой экспозиции

    // ── Пороги концентрации ──
    double max_symbol_concentration{0.25};     ///< Макс доля капитала на один символ
    double max_strategy_concentration{0.35};   ///< Макс доля капитала на одну стратегию

    // ── Пороги капитала ──
    double capital_exhaustion_threshold{0.90}; ///< Порог исчерпания капитала [0,1]

    // ── Веса скоринга ──
    double weight_conviction{0.35};            ///< Вес conviction в composite score
    double weight_net_edge{0.35};              ///< Вес net edge
    double weight_capital_efficiency{0.15};    ///< Вес capital efficiency
    double weight_urgency{0.15};               ///< Вес urgency

    // ── Масштабирование expected return ──
    double conviction_to_bps_scale{120.0};     ///< Масштаб: conviction 1.0 → N bps

    // ── Upgrade ──
    double upgrade_min_edge_advantage_bps{10.0}; ///< Мин разница edge для Upgrade vs худшей позиции

    // ── Drawdown penalty ──
    double drawdown_penalty_scale{0.5};        ///< Множитель: +X к порогу за каждые 5% просадки
};

/// Реализация на основе правил (production-grade)
class RuleBasedOpportunityCost : public IOpportunityCostEngine {
public:
    RuleBasedOpportunityCost(OpportunityCostConfig config,
                             std::shared_ptr<logging::ILogger> logger,
                             std::shared_ptr<clock::IClock> clock,
                             std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    OpportunityCostResult evaluate(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const PortfolioContext& portfolio_ctx,
        double conviction_threshold
    ) override;

private:
    /// Рассчитать балл возможности с полной декомпозицией
    OpportunityScore compute_score(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const PortfolioContext& portfolio_ctx) const;

    /// Определить действие и причину
    std::pair<OpportunityAction, OpportunityReason> determine_action(
        const OpportunityScore& score,
        const PortfolioContext& portfolio_ctx,
        double conviction,
        double conviction_threshold) const;

    /// Построить структурированные факторы решения
    OpportunityCostFactors build_factors(
        const strategy::TradeIntent& intent,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const PortfolioContext& portfolio_ctx,
        double conviction_threshold,
        const OpportunityScore& score,
        OpportunityAction action,
        OpportunityReason reason) const;

    /// Эффективный conviction threshold с учётом просадки
    double effective_conviction_threshold(
        double base_threshold,
        const PortfolioContext& portfolio_ctx) const;

    OpportunityCostConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    // ── Метрики ──
    std::shared_ptr<metrics::ICounter> actions_execute_;
    std::shared_ptr<metrics::ICounter> actions_defer_;
    std::shared_ptr<metrics::ICounter> actions_suppress_;
    std::shared_ptr<metrics::ICounter> actions_upgrade_;
    std::shared_ptr<metrics::IGauge> last_net_edge_bps_;
    std::shared_ptr<metrics::IGauge> last_score_;
    std::shared_ptr<metrics::IHistogram> decision_latency_;

    mutable std::mutex mutex_;
};

} // namespace tb::opportunity_cost
