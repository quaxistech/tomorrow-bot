#pragma once
#include "risk/risk_types.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "regime/regime_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <unordered_map>

namespace tb::governance { class GovernanceAuditLayer; }
namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::risk {

/// Интерфейс риск-движка
class IRiskEngine {
public:
    virtual ~IRiskEngine() = default;

    /// Оценить намерение с учётом всех факторов
    virtual RiskDecision evaluate(
        const strategy::TradeIntent& intent,
        const portfolio_allocator::SizingResult& sizing,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const uncertainty::UncertaintySnapshot& uncertainty
    ) = 0;

    /// Активировать аварийный выключатель
    virtual void activate_kill_switch(const std::string& reason) = 0;

    /// Деактивировать аварийный выключатель
    virtual void deactivate_kill_switch() = 0;

    /// Проверить состояние аварийного выключателя
    virtual bool is_kill_switch_active() const = 0;

    /// Зафиксировать отправку ордера (для rate limiting)
    virtual void record_order_sent() = 0;

    /// Зафиксировать результат сделки (для consecutive losses)
    virtual void record_trade_result(bool is_loss) = 0;

    /// Оценить состояние открытой позиции (intra-trade monitoring)
    virtual IntraTradeAssessment evaluate_position(
        const portfolio::Position& position,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features
    ) = 0;

    /// Зафиксировать закрытие сделки с деталями (для per-strategy budgets)
    virtual void record_trade_close(const StrategyId& strategy_id,
                                     const Symbol& symbol,
                                     double realized_pnl) = 0;

    /// Установить текущий режим рынка (для regime-aware limits)
    virtual void set_current_regime(regime::DetailedRegime regime) = 0;

    /// Получить снимок состояния риск-системы (observability)
    virtual RiskSnapshot get_risk_snapshot() const = 0;

    /// Сброс дневных счётчиков
    virtual void reset_daily() = 0;
};

/// Продакшн-реализация риск-движка с 23 правилами
class ProductionRiskEngine : public IRiskEngine {
public:
    ProductionRiskEngine(ExtendedRiskConfig config,
                         std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<clock::IClock> clock,
                         std::shared_ptr<metrics::IMetricsRegistry> metrics,
                         std::shared_ptr<governance::GovernanceAuditLayer> governance = nullptr);

    RiskDecision evaluate(
        const strategy::TradeIntent& intent,
        const portfolio_allocator::SizingResult& sizing,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const uncertainty::UncertaintySnapshot& uncertainty
    ) override;

    void activate_kill_switch(const std::string& reason) override;
    void deactivate_kill_switch() override;
    bool is_kill_switch_active() const override;
    void record_order_sent() override;
    void record_trade_result(bool is_loss) override;

    IntraTradeAssessment evaluate_position(
        const portfolio::Position& position,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features
    ) override;

    void record_trade_close(const StrategyId& strategy_id,
                             const Symbol& symbol,
                             double realized_pnl) override;

    void set_current_regime(regime::DetailedRegime regime) override;

    RiskSnapshot get_risk_snapshot() const override;

    void reset_daily() override;

private:
    // === Существующие 15 проверок ===

    /// Проверка: Аварийный выключатель
    void check_kill_switch(RiskDecision& decision) const;

    /// Проверка: Макс дневной убыток
    void check_max_daily_loss(const portfolio::PortfolioSnapshot& portfolio,
                              RiskDecision& decision) const;

    /// Проверка: Макс просадка
    void check_max_drawdown(const portfolio::PortfolioSnapshot& portfolio,
                            RiskDecision& decision) const;

    /// Проверка: Макс одновременных позиций
    void check_max_positions(const portfolio::PortfolioSnapshot& portfolio,
                             RiskDecision& decision) const;

    /// Проверка: Макс валовая экспозиция
    void check_max_exposure(const portfolio::PortfolioSnapshot& portfolio,
                            RiskDecision& decision) const;

    /// Проверка: Макс номинал позиции
    void check_max_notional(const portfolio_allocator::SizingResult& sizing,
                            RiskDecision& decision) const;

    /// Проверка: Макс плечо
    void check_max_leverage(const portfolio::PortfolioSnapshot& portfolio,
                            RiskDecision& decision) const;

    /// Проверка: Макс проскальзывание
    void check_max_slippage(const execution_alpha::ExecutionAlphaResult& exec_alpha,
                            RiskDecision& decision) const;

    /// Проверка: Частота ордеров
    void check_order_rate(RiskDecision& decision);

    /// Проверка: Подряд убыточные сделки
    void check_consecutive_losses(const portfolio::PortfolioSnapshot& portfolio,
                                   RiskDecision& decision) const;

    /// Проверка: Актуальность данных
    void check_stale_feed(const features::FeatureSnapshot& features,
                          RiskDecision& decision) const;

    /// Проверка: Качество стакана
    void check_book_quality(const features::FeatureSnapshot& features,
                            RiskDecision& decision) const;

    /// Проверка: Ширина спреда
    void check_spread(const features::FeatureSnapshot& features,
                      RiskDecision& decision) const;

    /// Проверка: Минимальная ликвидность
    void check_liquidity(const features::FeatureSnapshot& features,
                         RiskDecision& decision) const;

    /// Проверка: Макс убыток на сделку (блокирует ордера если текущие позиции в убытке)
    void check_max_loss_per_trade(const portfolio::PortfolioSnapshot& portfolio,
                                  RiskDecision& decision) const;

    // === Новые проверки 16-23 ===

    /// Проверка 16: Бюджет стратегии
    void check_strategy_budget(const strategy::TradeIntent& intent,
                               const portfolio::PortfolioSnapshot& portfolio,
                               RiskDecision& decision);

    /// Проверка 17: Концентрация символа
    void check_symbol_concentration(const strategy::TradeIntent& intent,
                                    const portfolio::PortfolioSnapshot& portfolio,
                                    RiskDecision& decision) const;

    /// Проверка 18: Однонаправленные позиции
    void check_same_direction_positions(const strategy::TradeIntent& intent,
                                        const portfolio::PortfolioSnapshot& portfolio,
                                        RiskDecision& decision) const;

    /// Проверка 19: UTC cutoff
    void check_utc_cutoff(RiskDecision& decision) const;

    /// Проверка 20: Оборачиваемость (turnover rate)
    void check_turnover_rate(RiskDecision& decision);

    /// Проверка 21: Реализованный дневной убыток
    void check_realized_daily_loss(const portfolio::PortfolioSnapshot& portfolio,
                                   RiskDecision& decision) const;

    /// Проверка 22: Интервал между сделками одного символа
    void check_trade_interval(const strategy::TradeIntent& intent,
                              RiskDecision& decision);

    /// Проверка 23: Масштабирование лимитов по режиму (модифицирует decision)
    void check_regime_scaled_limits(RiskDecision& decision) const;

    /// Проверка 24: Лимиты неопределённости — ужесточение max_notional при High/Extreme
    void check_uncertainty_limits(const uncertainty::UncertaintySnapshot& uncertainty,
                                  const portfolio_allocator::SizingResult& sizing,
                                  RiskDecision& decision) const;

    /// Проверка 25: Кулдаун неопределённости — троттлинг новых сделок при активном cooldown
    void check_uncertainty_cooldown(const uncertainty::UncertaintySnapshot& uncertainty,
                                    RiskDecision& decision) const;

    /// Проверка 26: Режим исполнения неопределённости — запрет новых входов при HaltNewEntries
    void check_uncertainty_execution_mode(const uncertainty::UncertaintySnapshot& uncertainty,
                                          const strategy::TradeIntent& intent,
                                          RiskDecision& decision) const;

    /// Проверка 27: Spot-семантика — SELL без открытой long позиции невозможен
    void check_spot_sell_without_position(const strategy::TradeIntent& intent,
                                          const portfolio::PortfolioSnapshot& portfolio,
                                          RiskDecision& decision) const;

    ExtendedRiskConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<governance::GovernanceAuditLayer> governance_;

    std::atomic<bool> kill_switch_active_{false};
    std::string kill_switch_reason_;
    // Note: consecutive_losses теперь берётся из portfolio snapshot
    std::deque<int64_t> order_timestamps_; ///< Временные метки ордеров для rate limiting

    // === Новое состояние ===
    std::unordered_map<std::string, StrategyRiskBudget> strategy_budgets_; ///< Бюджеты стратегий
    std::deque<int64_t> trade_close_timestamps_;                           ///< Для отслеживания оборачиваемости
    std::unordered_map<std::string, int64_t> last_trade_per_symbol_;       ///< Для интервалов между сделками
    regime::DetailedRegime current_regime_{regime::DetailedRegime::Undefined};
    double regime_scale_factor_{1.0};                                       ///< Кэшированный множитель режима

    mutable std::mutex mutex_;
};

} // namespace tb::risk
