#include <catch2/catch_test_macros.hpp>

#include "scanner/pair_filter.hpp"

using namespace tb::scanner;

namespace {

MarketSnapshot make_snapshot() {
    MarketSnapshot snapshot;
    snapshot.symbol = "BTCUSDT";
    snapshot.last_price = 100.0;
    snapshot.price_precision = 2;
    snapshot.status = "online";
    snapshot.orderbook.bids = {{99.99, 200.0}, {99.98, 150.0}};
    snapshot.orderbook.asks = {{100.01, 200.0}, {100.02, 150.0}};
    return snapshot;
}

SymbolFeatures make_features(double atr, double realized_vol_pct) {
    SymbolFeatures features;
    features.liquidity.volume_24h_usdt = 2'000'000.0;
    features.liquidity.open_interest_usdt = 1'500'000.0;
    features.liquidity.bid_depth_5_levels = 30'000.0;
    features.liquidity.ask_depth_5_levels = 30'000.0;
    features.spread.spread_bps = 2.0;
    features.anomaly.micro_noise_level = 0.1;
    features.volatility.atr = atr;
    features.volatility.realized_vol_pct = realized_vol_pct;
    return features;
}

} // namespace

TEST_CASE("PairFilter rejects pairs when ATR is below the economic floor", "[scanner][pair_filter]") {
    ScannerConfig cfg;
    cfg.min_volatility_pct = 0.10;
    cfg.candle_interval = "1m";

    PairFilter filter(cfg);
    auto verdict = filter.evaluate(make_snapshot(), make_features(0.12, 0.30), {});

    REQUIRE(verdict.reason == FilterReason::LowVolatility);
    REQUIRE(verdict.details.find("atr_pct=") != std::string::npos);
    REQUIRE(verdict.details.find("min_atr_pct=") != std::string::npos);
}

TEST_CASE("PairFilter scales ATR floor to the scanner candle interval", "[scanner][pair_filter]") {
    ScannerConfig cfg;
    cfg.min_volatility_pct = 0.10;
    cfg.candle_interval = "5m";

    PairFilter filter(cfg);
    auto verdict = filter.evaluate(make_snapshot(), make_features(0.20, 0.30), {});

    REQUIRE(verdict.reason == FilterReason::LowVolatility);
    REQUIRE(verdict.details.find("candle_interval=5m") != std::string::npos);
}

TEST_CASE("PairFilter allows pairs when ATR clears the economic floor", "[scanner][pair_filter]") {
    ScannerConfig cfg;
    cfg.min_volatility_pct = 0.10;
    cfg.candle_interval = "1m";

    PairFilter filter(cfg);
    auto verdict = filter.evaluate(make_snapshot(), make_features(0.20, 0.30), {});

    REQUIRE(verdict.reason == FilterReason::Passed);
    REQUIRE(verdict.passed());
}