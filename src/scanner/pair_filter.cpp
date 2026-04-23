#include "pair_filter.hpp"
#include "common/constants.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace tb::scanner {

namespace {

double candle_interval_minutes(std::string_view interval) {
    std::size_t digits = 0;
    while (digits < interval.size() &&
           std::isdigit(static_cast<unsigned char>(interval[digits])) != 0) {
        ++digits;
    }

    if (digits == 0 || digits == interval.size()) {
        return 1.0;
    }

    double magnitude = 0.0;
    try {
        magnitude = std::stod(std::string(interval.substr(0, digits)));
    } catch (...) {
        return 1.0;
    }

    if (magnitude <= 0.0) {
        return 1.0;
    }

    switch (interval[digits]) {
        case 'm': return magnitude;
        case 'H':
        case 'h': return magnitude * 60.0;
        case 'D':
        case 'd': return magnitude * 24.0 * 60.0;
        case 'W':
        case 'w': return magnitude * 7.0 * 24.0 * 60.0;
        default: return 1.0;
    }
}

double scanner_min_atr_pct(const ScannerConfig& cfg) {
    constexpr double kRoundTripTakerFeePct = common::fees::kDefaultTakerFeePct * 2.0 * 100.0;
    constexpr double kRuntimeEconomicAtrFloorPct = kRoundTripTakerFeePct * 1.25;

    // Runtime blocks entries on 1m ATR. Scanner may analyze wider candles, so the
    // minimum ATR% must be scaled to the scanner timeframe to stay economically equivalent.
    double interval_minutes = std::max(1.0, candle_interval_minutes(cfg.candle_interval));
    double timeframe_adjusted_floor_pct = kRuntimeEconomicAtrFloorPct * std::sqrt(interval_minutes);
    return std::max(cfg.min_volatility_pct, timeframe_adjusted_floor_pct);
}

} // namespace

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

    // Minimum price (reject micro-price contracts with poor scalping economics)
    if (snapshot.last_price > 0.0 && snapshot.last_price < cfg_.min_price_usdt) {
        return {FilterReason::PriceTooLow,
                "price=" + std::to_string(snapshot.last_price) +
                " < min=" + std::to_string(cfg_.min_price_usdt)};
    }

    // Tick-to-fee economics: reject if single tick in bps is too large for precise entries
    if (snapshot.last_price > 0.0 && snapshot.price_precision > 0) {
        double tick_size = std::pow(10.0, -snapshot.price_precision);
        double tick_value_bps = (tick_size / snapshot.last_price) * 10000.0;
        if (tick_value_bps > cfg_.max_tick_value_bps) {
            return {FilterReason::PoorTickEconomics,
                    "tick_bps=" + std::to_string(tick_value_bps) +
                    " > max=" + std::to_string(cfg_.max_tick_value_bps)};
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

    // Thin orderbook (skip when orderbook is ticker-derived with < 2 levels)
    double total_depth = features.liquidity.bid_depth_5_levels + features.liquidity.ask_depth_5_levels;
    bool has_real_book = (snapshot.orderbook.bids.size() >= 2 || snapshot.orderbook.asks.size() >= 2);
    if (has_real_book && total_depth < cfg_.min_orderbook_depth_usdt) {
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

    if (snapshot.last_price > 0.0 && features.volatility.atr > 0.0) {
        constexpr double kRoundTripTakerFeePct = common::fees::kDefaultTakerFeePct * 2.0 * 100.0;
        double atr_pct = features.volatility.atr / snapshot.last_price * 100.0;
        double min_atr_pct = scanner_min_atr_pct(cfg_);
        if (atr_pct < min_atr_pct) {
            return {FilterReason::LowVolatility,
                    "atr_pct=" + std::to_string(atr_pct) +
                    "% < min_atr_pct=" + std::to_string(min_atr_pct) +
                    "% (round_trip_fee_pct=" + std::to_string(kRoundTripTakerFeePct) +
                    "%, candle_interval=" + cfg_.candle_interval + ")"};
        }
    }

    // ИСПРАВЛЕНИЕ H10: 24h price change фильтр (YAML scorer_filter_* → ScannerConfig)
    // Exhausted pump/dump: позиции, прошедшие >X% 24h имеют повышенный риск разворота.
    if (snapshot.change_24h_pct != 0.0) {
        if (snapshot.change_24h_pct < cfg_.filter_min_change_24h) {
            return {FilterReason::ExtremeVolatility,
                    "change_24h=" + std::to_string(static_cast<int>(snapshot.change_24h_pct)) +
                    "% < min=" + std::to_string(static_cast<int>(cfg_.filter_min_change_24h)) + "%"};
        }
        if (snapshot.change_24h_pct > cfg_.filter_max_change_24h) {
            return {FilterReason::ExtremeVolatility,
                    "change_24h=" + std::to_string(static_cast<int>(snapshot.change_24h_pct)) +
                    "% > max=" + std::to_string(static_cast<int>(cfg_.filter_max_change_24h)) + "%"};
        }
    }

    return {FilterReason::Passed, ""};
}

} // namespace tb::scanner
