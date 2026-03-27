#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "strategy/strategy_config.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace tb::strategy {

/// Стратегия Breakout: сжатие BB → расширение + пробой
class BreakoutStrategy : public IStrategy {
public:
    BreakoutStrategy(std::shared_ptr<logging::ILogger> logger,
                     std::shared_ptr<clock::IClock> clock,
                     BreakoutConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    BreakoutConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};

    /// Буфер истории BB bandwidth для обнаружения сжатия→расширения
    std::deque<double> bandwidth_history_;
    std::mutex history_mutex_;
};

} // namespace tb::strategy
