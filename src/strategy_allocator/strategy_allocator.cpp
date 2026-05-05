#include "strategy_allocator/strategy_allocator.hpp"
#include <algorithm>
#include <cmath>
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
                    double regime_multiplier = hint.weight_multiplier;
                    if (!std::isfinite(regime_multiplier)) regime_multiplier = 0.0;
                    alloc.weight *= regime_multiplier;
                    alloc.reason = "Режим: " + hint.reason;
                }
                break;
            }
        }

        // 2. Применяем пригодность из мировой модели
        for (const auto& suit : world.strategy_suitability) {
            if (suit.strategy_id.get() == sid) {
                double world_model_multiplier = suit.suitability;
                if (!std::isfinite(world_model_multiplier)) world_model_multiplier = 0.0;
                alloc.weight *= world_model_multiplier;
                if (!alloc.reason.empty()) alloc.reason += " | ";
                alloc.reason += "Мир: " + suit.reason;
                break;
            }
        }

        // 3. Применяем множитель неопределённости
        double uncertainty_multiplier = uncertainty.size_multiplier;
        if (!std::isfinite(uncertainty_multiplier)) uncertainty_multiplier = 0.0;
        alloc.weight *= uncertainty_multiplier;
        alloc.size_multiplier = uncertainty_multiplier;

        // 4. Отключаем стратегии с очень низким весом.
        // Порог 0.00001: при micro-cap торговле uncertainty multiplier
        // бывает очень мал (0.002), а world suitability 0.3 для Unknown,
        // итого weight = base * 0.3 * 0.002 = 0.0006 — это рабочий вес.
        // Блокировать только совсем нулевые.
        if (alloc.weight < 0.00001) {
            alloc.is_enabled = false;
            if (!alloc.reason.empty()) alloc.reason += " | ";
            alloc.reason += "Вес ниже порога (< 0.00001)";
        }

        result.allocations.push_back(std::move(alloc));
    }

    // 5. Нормализуем веса активных стратегий
    // A5 fix: при единственной стратегии НЕ нормализуем.
    // Нормализация при одной стратегии всегда даёт weight=1.0,
    // что уничтожает модификаторы regime/world/uncertainty.
    // При одной стратегии абсолютный вес (0..1) несёт сигнал:
    // если world/regime/uncertainty «не одобряют» — weight < 1,
    // и downstream decision engine корректно ослабляет weighted_score.
    double total_weight = 0.0;
    for (const auto& a : result.allocations) {
        if (a.is_enabled) {
            total_weight += a.weight;
        }
    }

    result.enabled_count = 0;
    if (total_weight > 0.0) {
        int active_count = 0;
        for (const auto& a : result.allocations) {
            if (a.is_enabled) ++active_count;
        }
        for (auto& a : result.allocations) {
            if (a.is_enabled) {
                if (active_count > 1) {
                    a.weight /= total_weight;
                }
                // При active_count == 1: оставляем weight как есть (абсолютный модификатор)
                ++result.enabled_count;
            }
        }
    }

    // Пояснение
    explanation << "Аллокация: " << result.enabled_count << " из " << strategies.size()
                << " стратегий активны, uncertainty_size_mult="
                << uncertainty.size_multiplier;
    result.explanation = explanation.str();

    // Если все стратегии отключены — логируем причины для диагностики
    if (result.enabled_count == 0) {
        for (const auto& a : result.allocations) {
            logger_->info("Allocator", "Стратегия отключена",
                {{"id", a.strategy_id.get()},
                 {"weight", std::to_string(a.weight)},
                 {"enabled", a.is_enabled ? "true" : "false"},
                 {"reason", a.reason}});
        }
        logger_->info("Allocator", "Все стратегии отключены!",
            {{"uncertainty_size_mult", std::to_string(uncertainty.size_multiplier)},
             {"enabled", std::to_string(result.enabled_count)},
             {"total", std::to_string(strategies.size())}});
    }

    logger_->debug("Allocator", result.explanation,
                   {{"enabled", std::to_string(result.enabled_count)},
                    {"total", std::to_string(strategies.size())}});

    return result;
}

} // namespace tb::strategy_allocator
