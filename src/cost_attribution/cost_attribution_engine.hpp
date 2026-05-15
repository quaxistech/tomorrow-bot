#pragma once
/**
 * @file cost_attribution_engine.hpp
 * @brief Движок атрибуции издержек — декомпозиция PnL на компоненты
 */
#include "cost_attribution/cost_attribution_types.hpp"
#include "common/result.hpp"

#include <mutex>
#include <vector>

namespace tb::cost_attribution {

class CostAttributionEngine {
public:
    CostAttributionEngine() = default;

    /// Записать breakdown по одной сделке
    void record_trade(TradeCostBreakdown breakdown);

    /// Получить агрегированный отчёт за период
    [[nodiscard]] CostAttributionSummary summarize(Timestamp from, Timestamp to) const;

    /// Получить агрегированный отчёт по всем записанным сделкам
    [[nodiscard]] CostAttributionSummary summarize_all() const;

    /// Количество записанных сделок
    [[nodiscard]] std::size_t trade_count() const;

    /// Очистить историю
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<TradeCostBreakdown> trades_;

    [[nodiscard]] CostAttributionSummary aggregate(
        const std::vector<TradeCostBreakdown>& trades) const;
};

} // namespace tb::cost_attribution
