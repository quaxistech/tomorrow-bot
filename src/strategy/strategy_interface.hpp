#pragma once
#include "strategy/strategy_types.hpp"
#include <memory>
#include <optional>

namespace tb::strategy {

/// Базовый интерфейс торговой стратегии
/// СТРАТЕГИИ НЕ РАЗМЕЩАЮТ ОРДЕРА — они только предлагают торговые намерения
class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// Метаданные стратегии
    virtual StrategyMeta meta() const = 0;

    /// Обработка нового контекста → торговое намерение (или пусто)
    virtual std::optional<TradeIntent> evaluate(const StrategyContext& context) = 0;

    /// Стратегия активна (не отключена, не в ошибке)?
    virtual bool is_active() const = 0;

    /// Активировать/деактивировать стратегию
    virtual void set_active(bool active) = 0;

    /// Сбросить внутреннее состояние (для replay)
    virtual void reset() = 0;

    /// Уведомить, что pipeline отклонил вход (sizing/risk/exchange rejection)
    virtual void notify_entry_rejected() {}

    /// Уведомить о фактическом открытии позиции (feedback от pipeline)
    virtual void notify_position_opened(double /*entry_price*/, double /*size*/,
                                         Side /*side*/, PositionSide /*pos_side*/) {}

    /// Уведомить о закрытии позиции
    virtual void notify_position_closed() {}
};

} // namespace tb::strategy
