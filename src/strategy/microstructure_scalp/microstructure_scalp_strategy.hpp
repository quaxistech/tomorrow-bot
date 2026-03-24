#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

/// Стратегия Microstructure Scalp: дисбаланс стакана + агрессивный поток
class MicrostructureScalpStrategy : public IStrategy {
public:
    MicrostructureScalpStrategy(std::shared_ptr<logging::ILogger> logger,
                                std::shared_ptr<clock::IClock> clock);

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
