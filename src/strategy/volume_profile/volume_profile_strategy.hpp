#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "strategy/strategy_config.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

/// Стратегия Volume Profile Reversion: вход вблизи ключевых уровней VP
/// Логика: цена находится вблизи POC (Point of Control) или границ Value Area +
/// RSI подтверждает + ADX не слишком высокий (нет сильного тренда)
/// POC — магнит цены, Value Area — зона наибольшей торговой активности
class VolumeProfileStrategy : public IStrategy {
public:
    VolumeProfileStrategy(std::shared_ptr<logging::ILogger> logger,
                          std::shared_ptr<clock::IClock> clock,
                          VolumeProfileConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    VolumeProfileConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
