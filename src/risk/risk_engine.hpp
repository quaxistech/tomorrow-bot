#pragma once
#include "risk/risk_types.hpp"
#include "risk/risk_context.hpp"
#include "risk/policies/i_risk_check.hpp"
#include "risk/state/risk_state.hpp"
#include "risk/sizing/position_sizer.hpp"
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
#include <vector>
#include <atomic>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::risk {

/// Интерфейс риск-движка (backward-compatible)
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

/// Policy-based риск-движок с 33 проверками
class ProductionRiskEngine : public IRiskEngine {
public:
    ProductionRiskEngine(ExtendedRiskConfig config,
                         std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<clock::IClock> clock,
                         std::shared_ptr<metrics::IMetricsRegistry> metrics);

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
    /// Инициализировать цепочку policy-проверок
    void init_checks();

    /// Финализировать решение (min notional guard, summary, metrics)
    void finalize_decision(RiskDecision& decision,
                           const strategy::TradeIntent& intent,
                           const portfolio_allocator::SizingResult& sizing,
                           const portfolio::PortfolioSnapshot& portfolio);

    ExtendedRiskConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    /// Централизованное состояние (locks, PnL, drawdown, rates, streaks)
    RiskState state_;

    /// Position sizer
    PositionSizer sizer_;

    /// Цепочка проверок (33 policy checks выполняются последовательно)
    std::vector<std::unique_ptr<IRiskCheck>> checks_;

    /// Режим и kill switch
    std::atomic<bool> kill_switch_active_{false};
    std::string kill_switch_reason_;
    regime::DetailedRegime current_regime_{regime::DetailedRegime::Undefined};
    std::atomic<double> regime_scale_factor_{1.0};

    mutable std::mutex mutex_;
};

} // namespace tb::risk
