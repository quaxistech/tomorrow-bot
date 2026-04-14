#include "feature_calculator.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::scanner {

SymbolFeatures FeatureCalculator::compute(const MarketSnapshot& snapshot) const {
    SymbolFeatures f;
    f.liquidity = compute_liquidity(snapshot);
    f.spread = compute_spread(snapshot);
    f.volatility = compute_volatility(snapshot);
    f.orderbook = compute_orderbook(snapshot);
    f.trend_quality = compute_trend_quality(snapshot);
    f.anomaly = compute_anomaly(snapshot);
    return f;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.1 Метрики ликвидности
// ═══════════════════════════════════════════════════════════════════════════════

LiquidityFeatures FeatureCalculator::compute_liquidity(const MarketSnapshot& s) const {
    LiquidityFeatures liq;
    liq.volume_24h_usdt = s.turnover_24h;
    liq.turnover_24h = s.turnover_24h;
    liq.open_interest_usdt = s.open_interest * s.last_price;  // base coin → USDT

    const auto& ob = s.orderbook;
    double mid = ob.mid_price();

    // Глубина вблизи mid (±depth_near_mid_pct%)
    if (mid > 0.0) {
        double range_pct = cfg_.depth_near_mid_pct / 100.0;
        double lo = mid * (1.0 - range_pct);
        double hi = mid * (1.0 + range_pct);

        for (const auto& lvl : ob.bids) {
            if (lvl.price >= lo)
                liq.bid_depth_near_mid += lvl.quantity * lvl.price;
        }
        for (const auto& lvl : ob.asks) {
            if (lvl.price <= hi)
                liq.ask_depth_near_mid += lvl.quantity * lvl.price;
        }
    }

    liq.total_depth_near_mid = liq.bid_depth_near_mid + liq.ask_depth_near_mid;

    // Глубина по уровням
    for (size_t i = 0; i < std::min(ob.bids.size(), size_t(10)); ++i) {
        double notional = ob.bids[i].quantity * ob.bids[i].price;
        if (i < 5) liq.bid_depth_5_levels += notional;
        liq.bid_depth_10_levels += notional;
    }
    for (size_t i = 0; i < std::min(ob.asks.size(), size_t(10)); ++i) {
        double notional = ob.asks[i].quantity * ob.asks[i].price;
        if (i < 5) liq.ask_depth_5_levels += notional;
        liq.ask_depth_10_levels += notional;
    }

    // Score: log-normalized volume + OI + depth
    // $50M+ = excellent, $10M = good, $1M = acceptable
    double vol_score = std::min(std::log1p(liq.volume_24h_usdt / 1'000'000.0) / std::log1p(50.0), 1.0);
    double oi_score = std::min(std::log1p(liq.open_interest_usdt / 500'000.0) / std::log1p(100.0), 1.0);
    double depth_score = std::min(liq.total_depth_near_mid / 200'000.0, 1.0);

    liq.score = vol_score * 0.5 + oi_score * 0.3 + depth_score * 0.2;

    return liq;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.2 Метрики стоимости входа (spread)
// ═══════════════════════════════════════════════════════════════════════════════

SpreadFeatures FeatureCalculator::compute_spread(const MarketSnapshot& s) const {
    SpreadFeatures sp;
    sp.absolute_spread = s.orderbook.spread();
    sp.spread_bps = s.orderbook.spread_bps();

    // Score: lower spread = higher score. Exponential decay.
    // 0 bps = 1.0, 5 bps = 0.93, 10 bps = 0.74, 20 bps = 0.37, 50 bps ≈ 0.03
    sp.score = std::exp(-sp.spread_bps / 15.0);

    return sp;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.3 Метрики краткосрочной волатильности
// ═══════════════════════════════════════════════════════════════════════════════

VolatilityFeatures FeatureCalculator::compute_volatility(const MarketSnapshot& s) const {
    VolatilityFeatures vol;

    const auto& candles = s.candles;
    if (candles.size() < 5) return vol;

    // ATR (Average True Range)
    int window = std::min(cfg_.volatility_window, static_cast<int>(candles.size()) - 1);
    double atr_sum = 0.0;
    for (int i = static_cast<int>(candles.size()) - window; i < static_cast<int>(candles.size()); ++i) {
        double tr = candles[i].high - candles[i].low;
        if (i > 0) {
            tr = std::max(tr, std::abs(candles[i].high - candles[i-1].close));
            tr = std::max(tr, std::abs(candles[i].low - candles[i-1].close));
        }
        atr_sum += tr;
    }
    vol.atr = (window > 0) ? atr_sum / window : 0.0;

    // Realized volatility: std dev of returns
    std::vector<double> returns;
    for (size_t i = 1; i < candles.size(); ++i) {
        if (candles[i-1].close > 0.0)
            returns.push_back((candles[i].close - candles[i-1].close) / candles[i-1].close);
    }

    if (returns.size() >= 2) {
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double sq_sum = 0.0;
        for (double r : returns) sq_sum += (r - mean) * (r - mean);
        vol.realized_vol_pct = std::sqrt(sq_sum / (returns.size() - 1)) * 100.0;
    }

    // Range %
    double mid = s.orderbook.mid_price();
    if (mid > 0.0 && !candles.empty()) {
        size_t lookback = std::min(candles.size(), size_t(20));
        double hi = -1e18, lo = 1e18;
        for (size_t i = candles.size() - lookback; i < candles.size(); ++i) {
            hi = std::max(hi, candles[i].high);
            lo = std::min(lo, candles[i].low);
        }
        vol.range_pct = (hi - lo) / mid * 100.0;
    }

    // Volatility to spread ratio (higher = more tradeable for scalping)
    double spread = s.orderbook.spread();
    if (spread > 0.0 && vol.atr > 0.0) {
        vol.vol_to_spread_ratio = vol.atr / spread;
    }

    // Price velocity: absolute average candle change
    if (candles.size() >= 2 && mid > 0.0) {
        double total_change = 0.0;
        size_t lb = std::min(candles.size(), size_t(10));
        for (size_t i = candles.size() - lb; i < candles.size(); ++i) {
            total_change += std::abs(candles[i].close - candles[i].open);
        }
        vol.price_velocity = (total_change / lb) / mid * 10000.0; // bps per candle
    }

    // Impulse detection: 3+ consecutive candles in same direction with acceleration
    if (candles.size() >= 5) {
        size_t tail = candles.size();
        int consecutive = 0;
        double impulse_move = 0.0;
        for (size_t j = tail - 1; j > 0 && j >= tail - 5; --j) {
            double change = candles[j].close - candles[j].open;
            if (j == tail - 1) {
                impulse_move = change;
                consecutive = 1;
            } else if ((impulse_move > 0 && change > 0) || (impulse_move < 0 && change < 0)) {
                impulse_move += change;
                consecutive++;
            } else {
                break;
            }
        }
        vol.has_impulse = (consecutive >= 3);
        if (vol.has_impulse && mid > 0.0) {
            vol.impulse_quality = std::min(std::abs(impulse_move) / mid * 1000.0, 1.0);
        }
    }

    // Score: log-normal sweet spot for scalping — ATR/spread ratio centered at ~7x.
    // Based on Aldridge (2013): optimal vol/spread for scalping is 5-10x.
    // - ratio < 2: spread dominates profit → bad
    // - ratio 5-10: sweet spot, enough movement relative to entry cost
    // - ratio > 20: excessive slippage risk, unstable fills
    // Gaussian on log(ratio) gives a proper bell curve:
    if (vol.vol_to_spread_ratio > 0.0) {
        double log_ratio = std::log(vol.vol_to_spread_ratio);
        double log_optimal = std::log(7.0);   // peak at ATR = 7× spread
        double sigma = 0.8;                    // width of acceptable zone
        vol.score = std::exp(-(log_ratio - log_optimal) * (log_ratio - log_optimal)
                             / (2.0 * sigma * sigma));
    }

    return vol;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.4 Метрики стакана
// ═══════════════════════════════════════════════════════════════════════════════

OrderBookFeatures FeatureCalculator::compute_orderbook(const MarketSnapshot& s) const {
    OrderBookFeatures obf;
    const auto& ob = s.orderbook;

    if (ob.bids.empty() || ob.asks.empty()) return obf;

    // Imbalance at 5 and 10 levels
    auto calc_imbalance = [](const std::vector<OrderBookLevel>& bids,
                             const std::vector<OrderBookLevel>& asks,
                             size_t levels) {
        double bid_vol = 0.0, ask_vol = 0.0;
        for (size_t i = 0; i < std::min(bids.size(), levels); ++i)
            bid_vol += bids[i].quantity * bids[i].price;
        for (size_t i = 0; i < std::min(asks.size(), levels); ++i)
            ask_vol += asks[i].quantity * asks[i].price;
        double total = bid_vol + ask_vol;
        return (total > 0.0) ? (bid_vol - ask_vol) / total : 0.0;
    };

    obf.imbalance_5 = calc_imbalance(ob.bids, ob.asks, 5);
    obf.imbalance_10 = calc_imbalance(ob.bids, ob.asks, 10);

    // Density near mid: average notional per level in first 5 levels
    double total_5 = 0.0;
    size_t count_5 = std::min(ob.bids.size(), size_t(5)) + std::min(ob.asks.size(), size_t(5));
    for (size_t i = 0; i < std::min(ob.bids.size(), size_t(5)); ++i)
        total_5 += ob.bids[i].quantity * ob.bids[i].price;
    for (size_t i = 0; i < std::min(ob.asks.size(), size_t(5)); ++i)
        total_5 += ob.asks[i].quantity * ob.asks[i].price;
    obf.density_near_mid = (count_5 > 0) ? total_5 / count_5 : 0.0;

    // Large walls detection: single order > 20% of total 10-level depth
    double total_10 = 0.0;
    double max_single = 0.0;
    for (size_t i = 0; i < std::min(ob.bids.size(), size_t(10)); ++i) {
        double n = ob.bids[i].quantity * ob.bids[i].price;
        total_10 += n;
        max_single = std::max(max_single, n);
    }
    for (size_t i = 0; i < std::min(ob.asks.size(), size_t(10)); ++i) {
        double n = ob.asks[i].quantity * ob.asks[i].price;
        total_10 += n;
        max_single = std::max(max_single, n);
    }
    if (total_10 > 0.0) {
        obf.largest_wall_pct = max_single / total_10;
        obf.has_large_walls = (obf.largest_wall_pct > 0.20);
    }

    // Score: balance between depth, density, and reasonable imbalance
    double depth_component = std::min(total_5 / 100'000.0, 1.0);  // $100k = max
    double imbalance_penalty = std::abs(obf.imbalance_5) > 0.5
        ? (std::abs(obf.imbalance_5) - 0.5) * 0.5 : 0.0;
    double wall_penalty = obf.has_large_walls ? 0.15 : 0.0;

    obf.score = std::max(0.0, depth_component - imbalance_penalty - wall_penalty);

    return obf;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.5 Метрики качества движения
// ═══════════════════════════════════════════════════════════════════════════════

TrendQualityFeatures FeatureCalculator::compute_trend_quality(const MarketSnapshot& s) const {
    TrendQualityFeatures tq;

    const auto& candles = s.candles;
    if (candles.size() < 10) return tq;

    size_t lookback = std::min(candles.size(), size_t(20));
    size_t start = candles.size() - lookback;

    // Direction (+1 or -1 per candle)
    int up = 0, down = 0;
    double total_up = 0.0, total_down = 0.0;

    for (size_t i = start; i < candles.size(); ++i) {
        double change = candles[i].close - candles[i].open;
        if (change > 0) { up++; total_up += change; }
        else if (change < 0) { down++; total_down += std::abs(change); }
    }

    int total = up + down;
    if (total == 0) return tq;

    // Micro-trend direction: [-1, +1]
    tq.micro_trend_direction = static_cast<double>(up - down) / total;

    // Micro-trend strength: how dominant one direction is
    tq.micro_trend_strength = std::abs(tq.micro_trend_direction);

    // Pullback count: direction changes against dominant trend
    int pullbacks = 0;
    double sum_pullback_depth = 0.0;
    bool trend_up = (up > down);

    for (size_t i = start + 1; i < candles.size(); ++i) {
        double change = candles[i].close - candles[i].open;
        bool is_pullback = (trend_up && change < 0) || (!trend_up && change > 0);
        if (is_pullback) {
            pullbacks++;
            double mid = s.orderbook.mid_price();
            if (mid > 0.0)
                sum_pullback_depth += std::abs(change) / mid * 100.0;
        }
    }

    tq.pullback_count = pullbacks;
    tq.avg_pullback_depth_pct = (pullbacks > 0)
        ? sum_pullback_depth / pullbacks : 0.0;

    // Momentum persistence: how many consecutive candles sustain direction at end
    int consecutive = 0;
    for (auto it = candles.rbegin(); it != candles.rend() && consecutive < 10; ++it) {
        double change = it->close - it->open;
        if (consecutive == 0) {
            consecutive = 1;
        } else {
            double prev_change = (it - 1)->close - (it - 1)->open;
            if ((prev_change > 0 && change > 0) || (prev_change < 0 && change < 0)) {
                consecutive++;
            } else {
                break;
            }
        }
    }
    tq.momentum_persistence = std::min(consecutive / 5.0, 1.0);

    // Score: strong, consistent trend with shallow pullbacks = high score
    double trend_component = tq.micro_trend_strength * 0.4;
    double pullback_component = 0.0;
    if (tq.avg_pullback_depth_pct > 0.0) {
        pullback_component = std::max(0.0, 0.3 - tq.avg_pullback_depth_pct * 0.1);
    } else {
        pullback_component = 0.3;
    }
    double momentum_component = tq.momentum_persistence * 0.3;

    tq.score = trend_component + pullback_component + momentum_component;

    return tq;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5.3.6 Метрики аномальности
// ═══════════════════════════════════════════════════════════════════════════════

AnomalyFeatures FeatureCalculator::compute_anomaly(const MarketSnapshot& s) const {
    AnomalyFeatures af;

    const auto& candles = s.candles;
    if (candles.size() < 10) return af;

    // Volume spike: last candle volume vs average of previous N
    size_t lb = std::min(candles.size() - 1, size_t(20));
    double avg_vol = 0.0;
    for (size_t i = candles.size() - 1 - lb; i < candles.size() - 1; ++i) {
        avg_vol += candles[i].volume;
    }
    avg_vol /= lb;

    if (avg_vol > 0.0) {
        double last_vol = candles.back().volume;
        af.volume_spike_magnitude = last_vol / avg_vol;
        af.volume_spike = (af.volume_spike_magnitude > 3.0);
    }

    // OI + price divergence heuristic:
    // If price moved significantly but OI is low relative to volume → suspect
    if (s.open_interest > 0.0 && s.turnover_24h > 0.0) {
        double oi_to_vol = s.open_interest / s.turnover_24h;
        double price_change_abs = std::abs(s.change_24h_pct);
        // Low OI/volume ratio + big price change = potential manipulation
        if (oi_to_vol < 0.1 && price_change_abs > 5.0) {
            af.oi_price_divergence = true;
        }
    }

    // Directional instability: number of direction changes / total candles
    int direction_changes = 0;
    for (size_t i = candles.size() - lb; i + 1 < candles.size(); ++i) {
        double c1 = candles[i].close - candles[i].open;
        double c2 = candles[i+1].close - candles[i+1].open;
        if ((c1 > 0 && c2 < 0) || (c1 < 0 && c2 > 0)) direction_changes++;
    }
    af.directional_instability = (lb > 1)
        ? static_cast<double>(direction_changes) / (lb - 1) : 0.0;

    // Micro noise level: ratio of wicks to bodies
    double total_wick = 0.0, total_body = 0.0;
    for (size_t i = candles.size() - lb; i < candles.size(); ++i) {
        double body = std::abs(candles[i].close - candles[i].open);
        double range = candles[i].high - candles[i].low;
        total_body += body;
        total_wick += std::max(0.0, range - body);
    }
    af.micro_noise_level = (total_wick + total_body > 0.0)
        ? total_wick / (total_wick + total_body) : 0.0;

    // Anomaly score: higher = worse
    af.score = 0.0;
    if (af.volume_spike) af.score += 0.2;
    if (af.oi_price_divergence) af.score += 0.25;
    af.score += af.directional_instability * 0.3;
    af.score += af.micro_noise_level * 0.25;
    af.score = std::min(af.score, 1.0);

    return af;
}

} // namespace tb::scanner
