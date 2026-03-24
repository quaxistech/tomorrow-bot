#include "strategy_allocator/strategy_allocator.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>

namespace tb::strategy_allocator {

RegimeAwareAllocator::RegimeAwareAllocator(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
{}

AllocationResult RegimeAwareAllocator::allocate(
    const std::vector<std::shared_ptr<strategy::IStrategy>>& strategies,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world,
    const uncertainty::UncertaintySnapshot& uncertainty) {

    AllocationResult result;
    result.computed_at = clock_->now();
    std::ostringstream explanation;

    for (const auto& strat : strategies) {
        if (!strat) continue;

        auto meta = strat->meta();
        const auto& sid = meta.id.get();

        StrategyAllocation alloc;
        alloc.strategy_id = meta.id;
        alloc.weight = 1.0; // Начальный вес
        alloc.is_enabled = strat->is_active();

        if (!alloc.is_enabled) {
            alloc.weight = 0.0;
            alloc.reason = "Стратегия деактивирована";
            result.allocations.push_back(std::move(alloc));
            continue;
        }

        // 1. Применяем рекомендации режима
        for (const auto& hint : regime.strategy_hints) {
            if (hint.strategy_id.get() == sid) {
                if (!hint.should_enable) {
                    alloc.weight = 0.0;
                    alloc.reason = "Отключена режимом: " + hint.reason;
                } else {
                    alloc.weight *= hint.weight_multiplier;
                    alloc.reason = "Режим: " + hint.reason;
                }
                break;
            }
        }

        // 2. Применяем пригодность из мировой модели
        for (const auto& suit : world.strategy_suitability) {
            if (suit.strategy_id.get() == sid) {
                alloc.weight *= suit.suitability;
                if (!alloc.reason.empty()) alloc.reason += " | ";
                alloc.reason += "Мир: " + suit.reason;
                break;
            }
        }

        // 3. Применяем множитель неопределённости
        alloc.weight *= uncertainty.size_multiplier;
        alloc.size_multiplier = uncertainty.size_multiplier;

        // 4. Отключаем стратегии с низким весом
        if (alloc.weight < 0.1) {
            alloc.is_enabled = false;
            if (!alloc.reason.empty()) alloc.reason += " | ";
            alloc.reason += "Вес ниже порога (< 0.1)";
        }

        result.allocations.push_back(std::move(alloc));
    }

    // 5. Нормализуем веса активных стратегий
    double total_weight = 0.0;
    for (const auto& a : result.allocations) {
        if (a.is_enabled) {
            total_weight += a.weight;
        }
    }

    result.enabled_count = 0;
    if (total_weight > 0.0) {
        for (auto& a : result.allocations) {
            if (a.is_enabled) {
                a.weight /= total_weight;
                ++result.enabled_count;
            }
        }
    }

    // Пояснение
    explanation << "Аллокация: " << result.enabled_count << " из " << strategies.size()
                << " стратегий активны, uncertainty_size_mult="
                << uncertainty.size_multiplier;
    result.explanation = explanation.str();

    logger_->debug("Allocator", result.explanation,
                   {{"enabled", std::to_string(result.enabled_count)},
                    {"total", std::to_string(strategies.size())}});

    return result;
}

} // namespace tb::strategy_allocator
