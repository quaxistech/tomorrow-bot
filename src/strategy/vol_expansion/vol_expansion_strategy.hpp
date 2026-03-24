#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace tb::strategy {

/// Стратегия Vol Expansion: вход при смене волатильностного режима
class VolExpansionStrategy : public IStrategy {
public:
    VolExpansionStrategy(std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<clock::IClock> clock);

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    static constexpr std::size_t kAtrHistorySize = 10;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};

    /// Буфер ATR для обнаружения расширения
    std::deque<double> atr_history_;
    std::mutex history_mutex_;
};

} // namespace tb::strategy
