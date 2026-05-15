#pragma once
#include "strategy/strategy_interface.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tb::strategy {

/// Реестр стратегий — управляет регистрацией и доступом
class StrategyRegistry {
public:
    /// Зарегистрировать стратегию
    void register_strategy(std::shared_ptr<IStrategy> strategy);

    /// Удалить стратегию по идентификатору
    void unregister(const StrategyId& id);

    /// Получить стратегию по идентификатору
    std::shared_ptr<IStrategy> get(const StrategyId& id) const;

    /// Все зарегистрированные стратегии
    std::vector<std::shared_ptr<IStrategy>> all() const;

    /// Только активные стратегии
    std::vector<std::shared_ptr<IStrategy>> active() const;

    /// Количество зарегистрированных стратегий
    std::size_t count() const;

private:
    std::unordered_map<std::string, std::shared_ptr<IStrategy>> strategies_;
    mutable std::mutex mutex_;
};

} // namespace tb::strategy
