#pragma once
/**
 * @file pair_ranker.hpp
 * @brief Модель ранжирования пар (§8).
 *
 * Рассчитывает интегральный score на основе конфигурируемых весов.
 * Score интерпретируемый: бонусы и штрафы сохраняются отдельно.
 */

#include "scanner_types.hpp"
#include "scanner_config.hpp"

namespace tb::scanner {

class PairRanker {
public:
    explicit PairRanker(const ScannerConfig& cfg) : cfg_(cfg) {}

    /// Рассчитать итоговый рейтинг символа
    SymbolScore compute(const SymbolFeatures& features,
                        const TrapAggregateResult& traps,
                        const MarketSnapshot& snapshot) const;

private:
    const ScannerConfig& cfg_;
};

} // namespace tb::scanner
