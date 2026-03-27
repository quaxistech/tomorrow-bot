#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "strategy/strategy_config.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

/// Стратегия Momentum: EMA crossover + RSI + ADX подтверждение тренда
class MomentumStrategy : public IStrategy {
public:
    MomentumStrategy(std::shared_ptr<logging::ILogger> logger,
                     std::shared_ptr<clock::IClock> clock,
                     MomentumConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    MomentumConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
