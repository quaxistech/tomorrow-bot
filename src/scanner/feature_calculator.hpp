#pragma once
/**
 * @file feature_calculator.hpp
 * @brief Вычисление признаков пригодности пары для скальпинга (§5.3).
 */

#include "scanner_types.hpp"
#include "scanner_config.hpp"

namespace tb::scanner {

class FeatureCalculator {
public:
    explicit FeatureCalculator(const ScannerConfig& cfg) : cfg_(cfg) {}

    /// Вычислить все признаки по снимку рыночных данных
    SymbolFeatures compute(const MarketSnapshot& snapshot) const;

private:
    LiquidityFeatures compute_liquidity(const MarketSnapshot& s) const;
    SpreadFeatures compute_spread(const MarketSnapshot& s) const;
    VolatilityFeatures compute_volatility(const MarketSnapshot& s) const;
    OrderBookFeatures compute_orderbook(const MarketSnapshot& s) const;
    TrendQualityFeatures compute_trend_quality(const MarketSnapshot& s) const;
    AnomalyFeatures compute_anomaly(const MarketSnapshot& s) const;

    const ScannerConfig& cfg_;
};

} // namespace tb::scanner
