#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "strategy/strategy_config.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

/// Стратегия VWAP Reversion: вход при отклонении от VWAP с ожиданием возврата
/// Логика: цена отклонилась от trade VWAP (из microstructure features) +
/// RSI подтверждает перепроданность/перекупленность + ADX низкий (нет тренда) +
/// Volume confirmation
class VwapReversionStrategy : public IStrategy {
public:
    VwapReversionStrategy(std::shared_ptr<logging::ILogger> logger,
                          std::shared_ptr<clock::IClock> clock,
                          VwapReversionConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    VwapReversionConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
