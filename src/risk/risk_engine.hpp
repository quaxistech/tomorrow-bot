#pragma once
#include "risk/risk_types.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>

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
        const execution_alpha::ExecutionAlphaResult& exec_alpha
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
};

/// Продакшн-реализация риск-движка с 14 правилами
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
        const execution_alpha::ExecutionAlphaResult& exec_alpha
    ) override;

    void activate_kill_switch(const std::string& reason) override;
    void deactivate_kill_switch() override;
    bool is_kill_switch_active() const override;
    void record_order_sent() override;
    void record_trade_result(bool is_loss) override;

private:
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
    void check_consecutive_losses(RiskDecision& decision) const;

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

    ExtendedRiskConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    std::atomic<bool> kill_switch_active_{false};
    std::string kill_switch_reason_;
    int consecutive_losses_{0};
    std::deque<int64_t> order_timestamps_; ///< Временные метки ордеров для rate limiting
    mutable std::mutex mutex_;
};

} // namespace tb::risk
