#pragma once
/**
 * @file pair_filter.hpp
 * @brief Параметризуемые фильтры неподходящих пар (§7).
 */

#include "scanner_types.hpp"
#include "scanner_config.hpp"

namespace tb::scanner {

class PairFilter {
public:
    explicit PairFilter(const ScannerConfig& cfg) : cfg_(cfg) {}

    /// Проверить пару по всем фильтрам. Возвращает Passed если всё ОК.
    FilterVerdict evaluate(const MarketSnapshot& snapshot,
                           const SymbolFeatures& features,
                           const TrapAggregateResult& traps) const;

private:
    const ScannerConfig& cfg_;
};

} // namespace tb::scanner
