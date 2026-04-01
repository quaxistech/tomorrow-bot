#include "pair_filter.hpp"
#include <algorithm>

namespace tb::scanner {

FilterVerdict PairFilter::evaluate(const MarketSnapshot& snapshot,
                                   const SymbolFeatures& features,
                                   const TrapAggregateResult& traps) const {
    // §7: Minimum reasons for exclusion

    // Invalid data
    if (!snapshot.is_valid()) {
        return {FilterReason::InvalidData, "invalid_price_or_empty_orderbook"};
    }

    // Not online
    if (snapshot.status != "online" && snapshot.status != "normal") {
        return {FilterReason::NotOnline, "status=" + snapshot.status};
    }

    // Blacklist
    for (const auto& bl : cfg_.blacklist) {
        if (snapshot.symbol == bl) {
            return {FilterReason::Blacklisted, snapshot.symbol};
        }
    }

    // Low liquidity
    if (features.liquidity.volume_24h_usdt < cfg_.min_volume_usdt) {
        return {FilterReason::LowLiquidity,
                "vol_24h=" + std::to_string(static_cast<int64_t>(features.liquidity.volume_24h_usdt)) +
                " < min=" + std::to_string(static_cast<int64_t>(cfg_.min_volume_usdt))};
    }

    // Wide spread
    if (features.spread.spread_bps > cfg_.max_spread_bps) {
        return {FilterReason::WideSpread,
                "spread=" + std::to_string(static_cast<int>(features.spread.spread_bps)) +
                "bps > max=" + std::to_string(static_cast<int>(cfg_.max_spread_bps))};
    }

    // Low open interest
    if (features.liquidity.open_interest_usdt < cfg_.min_open_interest_usdt) {
        return {FilterReason::LowOpenInterest,
                "oi=" + std::to_string(static_cast<int64_t>(features.liquidity.open_interest_usdt)) +
                " < min=" + std::to_string(static_cast<int64_t>(cfg_.min_open_interest_usdt))};
    }

    // Thin orderbook
    double total_depth = features.liquidity.bid_depth_5_levels + features.liquidity.ask_depth_5_levels;
    if (total_depth < cfg_.min_orderbook_depth_usdt) {
        return {FilterReason::ThinOrderBook,
                "depth_5=" + std::to_string(static_cast<int64_t>(total_depth)) +
                " < min=" + std::to_string(static_cast<int64_t>(cfg_.min_orderbook_depth_usdt))};
    }

    // High trap risk
    if (traps.total_risk > cfg_.max_trap_risk) {
        return {FilterReason::HighTrapRisk,
                "trap_risk=" + std::to_string(static_cast<int>(traps.total_risk * 100)) +
                "% > max=" + std::to_string(static_cast<int>(cfg_.max_trap_risk * 100)) + "%"};
    }

    // High noise
    if (features.anomaly.micro_noise_level > cfg_.max_noise_level) {
        return {FilterReason::HighNoise,
                "noise=" + std::to_string(static_cast<int>(features.anomaly.micro_noise_level * 100)) +
                "% > max=" + std::to_string(static_cast<int>(cfg_.max_noise_level * 100)) + "%"};
    }

    // Extreme volatility (too high for safe scalping)
    if (features.volatility.realized_vol_pct > cfg_.max_volatility_pct) {
        return {FilterReason::ExtremeVolatility,
                "vol=" + std::to_string(static_cast<int>(features.volatility.realized_vol_pct * 100) / 100.0) +
                "% > max=" + std::to_string(static_cast<int>(cfg_.max_volatility_pct)) + "%"};
    }

    // Too low volatility (can't cover spread)
    if (features.volatility.realized_vol_pct < cfg_.min_volatility_pct &&
        features.volatility.realized_vol_pct > 0.0) {
        return {FilterReason::LowVolatility,
                "vol=" + std::to_string(features.volatility.realized_vol_pct) +
                "% < min=" + std::to_string(cfg_.min_volatility_pct) + "%"};
    }

    return {FilterReason::Passed, ""};
}

} // namespace tb::scanner
