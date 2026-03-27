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

/// Стратегия RSI Divergence: обнаружение расхождений между ценой и RSI
/// Бычья дивергенция: цена делает новый минимум, RSI — нет (ослабление продавцов)
/// Медвежья дивергенция: цена делает новый максимум, RSI — нет (ослабление покупателей)
class RsiDivergenceStrategy : public IStrategy {
public:
    RsiDivergenceStrategy(std::shared_ptr<logging::ILogger> logger,
                          std::shared_ptr<clock::IClock> clock,
                          RsiDivergenceConfig cfg = {});

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    struct PriceRsiPoint {
        double price;
        double rsi;
    };

    RsiDivergenceConfig cfg_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};

    /// Буфер price+RSI для поиска дивергенций
    std::deque<PriceRsiPoint> history_;
    std::mutex history_mutex_;
};

} // namespace tb::strategy
