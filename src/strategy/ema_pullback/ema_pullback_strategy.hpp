#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "strategy/strategy_config.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

/// Стратегия EMA Pullback: вход в направлении тренда после отката к EMA
/// Логика: EMA20 > EMA50 (тренд бычий) + цена откатилась к EMA20 + RSI в нейтральной зоне
/// + ADX подтверждает наличие тренда + momentum начинает восстанавливаться
class EmaPullbackStrategy : public IStrategy {
public:
    EmaPullbackStrategy(std::shared_ptr<logging::ILogger> logger,
                        std::shared_ptr<clock::IClock> clock,
                        EmaPullbackConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    EmaPullbackConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
