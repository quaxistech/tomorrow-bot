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

/// Конфигурация модуля opportunity cost (загружается из YAML).
///
/// Дефолты откалиброваны для USDT-M futures scalping на 1-минутных свечах.
/// Все доли и пороги экспозиции выражены как fraction [0, 1], не как проценты.
///
/// Научные основания:
///   - Kelly criterion (Kelly 1956): связь edge/variance с оптимальным sizing
///   - Optimal execution (Almgren & Chriss 2001): пороги edge > execution cost
///   - Futures risk management (Hull 2018): margin reserve, concentration limits
///   - Drawdown recovery (Thorp 2006, Vince 1992): half-Kelly при просадке
struct OpportunityCostConfig {
    // ── Пороги net edge (базисные пункты) ──
    // min_net_expected_bps ≥ средний half-spread (~0.5 bps на основных USDT-M парах).
    // Almgren & Chriss 2001: edge должен покрывать transaction costs.
    double min_net_expected_bps{1.0};          ///< Мин чистый ожидаемый доход для входа (bps)
    double execute_min_net_bps{3.0};           ///< Мин чистый доход для немедленного исполнения (bps)

    // ── Пороги экспозиции ──
    // 70% margin utilization оставляет 30% резерв для maintenance margin
    // и покрытия волатильности (Hull 2018, ch. 2: futures margin mechanics).
    double high_exposure_threshold{0.70};      ///< Порог высокой маржинальной экспозиции [0,1]
    double high_exposure_min_conviction{0.60}; ///< Мин conviction при высокой экспозиции

    // ── Пороги концентрации ──
    // Markowitz MPT: диверсификация снижает unsystematic risk.
    // Практика HFT risk: max 25-40% на один инструмент.
    double max_symbol_concentration{0.25};     ///< Макс маржинальная доля капитала на один символ [0,1]
    double max_strategy_concentration{0.35};   ///< Макс маржинальная доля капитала на одну стратегию [0,1]

    // ── Пороги капитала ──
    // 85% — 15% buffer для maintenance margin (~5-10% на Binance USDT-M)
    // + запас для adverse price movement.
    double capital_exhaustion_threshold{0.85}; ///< Порог исчерпания маржинального капитала [0,1]

    // ── Веса скоринга (composite score для аудита и телеметрии) ──
    // Сумма весов = 1.0. Conviction и net_edge — основные драйверы.
    double weight_conviction{0.35};            ///< Вес conviction в composite score
    double weight_net_edge{0.35};              ///< Вес net edge
    double weight_capital_efficiency{0.15};    ///< Вес capital efficiency
    double weight_urgency{0.15};              ///< Вес urgency

    // ── Масштабирование expected return ──
    // conviction 1.0 → 100 bps ожидаемого дохода на leveraged USDT-M scalp.
    // Типичный диапазон: 50-200 bps в зависимости от волатильности и плеча.
    double conviction_to_bps_scale{100.0};     ///< Масштаб: conviction 1.0 → N bps

    // ── Upgrade ──
    // Порог должен превышать 2× round-trip execution cost (~4 bps при maker fees),
    // чтобы ротация окупалась.
    double upgrade_min_edge_advantage_bps{5.0}; ///< Мин разница edge для Upgrade vs худшей позиции

    // ── Drawdown penalty ──
    // Thorp 2006: half-Kelly при значительной просадке.
    // +scale к conviction threshold за каждые 5% drawdown от пика.
    // Было 0.5 — при 5% DD это +0.50 к порогу (0.68→1.18 — невозможный!)
    // Теперь 0.05 — при 5% DD это +0.05 (0.50→0.55 — разумная осторожность)
    double drawdown_penalty_scale{0.05};       ///< Множитель: +X к порогу за каждые 5% просадки

    // ── Consecutive loss penalty ──
    // Серия убытков повышает required conviction для входа.
    double consecutive_loss_penalty{0.005};    ///< +X к порогу за каждый убыточный трейд подряд
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
