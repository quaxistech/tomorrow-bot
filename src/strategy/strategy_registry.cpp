#include "strategy/strategy_registry.hpp"
#include <algorithm>

namespace tb::strategy {

void StrategyRegistry::register_strategy(std::shared_ptr<IStrategy> strategy) {
    if (!strategy) return;
    auto id = strategy->meta().id.get();
    std::lock_guard lock(mutex_);
    strategies_[id] = std::move(strategy);
}

void StrategyRegistry::unregister(const StrategyId& id) {
    std::lock_guard lock(mutex_);
    strategies_.erase(id.get());
}

std::shared_ptr<IStrategy> StrategyRegistry::get(const StrategyId& id) const {
    std::lock_guard lock(mutex_);
    auto it = strategies_.find(id.get());
    if (it != strategies_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<IStrategy>> StrategyRegistry::all() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<IStrategy>> result;
    result.reserve(strategies_.size());
    for (const auto& [_, s] : strategies_) {
        result.push_back(s);
    }
    return result;
}

std::vector<std::shared_ptr<IStrategy>> StrategyRegistry::active() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<IStrategy>> result;
    for (const auto& [_, s] : strategies_) {
        if (s->is_active()) {
            result.push_back(s);
        }
    }
    return result;
}

std::size_t StrategyRegistry::count() const {
    std::lock_guard lock(mutex_);
    return strategies_.size();
}

} // namespace tb::strategy
