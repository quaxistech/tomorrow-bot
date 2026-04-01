#pragma once
/**
 * @file bias_detector.hpp
 * @brief Определение bias направления: LONG / SHORT / NEUTRAL (§9).
 */

#include "scanner_types.hpp"
#include "scanner_config.hpp"

namespace tb::scanner {

class BiasDetector {
public:
    explicit BiasDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    /// Определить bias и confidence для символа
    struct BiasResult {
        BiasDirection direction{BiasDirection::Neutral};
        double confidence{0.0};
        std::vector<std::string> reasons;
    };

    BiasResult detect(const MarketSnapshot& snapshot,
                      const SymbolFeatures& features,
                      const TrapAggregateResult& traps) const;

private:
    const ScannerConfig& cfg_;
};

} // namespace tb::scanner
