#include "indicators/advanced_indicators.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::indicators {

// ─────────────────────────────────────────────────────────────────────────────
// AnchoredVwap
// ─────────────────────────────────────────────────────────────────────────────

AnchoredVwap::AnchoredVwap(int64_t session_window_ns)
    : session_window_ns_(session_window_ns) {}

void AnchoredVwap::reset(int64_t new_anchor_ts_ns) {
    anchor_ts_ns_ = new_anchor_ts_ns;
    cum_pv_ = 0.0;
    cum_v_ = 0.0;
    cum_p2v_ = 0.0;
    sample_count_ = 0;
}

void AnchoredVwap::on_trade(double price, double volume, int64_t ts_ns) {
    if (!(price > 0.0) || !(volume > 0.0)) return;
    if (!std::isfinite(price) || !std::isfinite(volume)) return;

    // Init anchor on first trade.
    if (anchor_ts_ns_ == 0) {
        // Anchor at session boundary (floor to session window).
        anchor_ts_ns_ = (ts_ns / session_window_ns_) * session_window_ns_;
    }

    // Reset on new session window crossing.
    int64_t current_session = ts_ns / session_window_ns_;
    int64_t anchor_session = anchor_ts_ns_ / session_window_ns_;
    if (current_session > anchor_session) {
        reset(current_session * session_window_ns_);
    }

    cum_pv_  += price * volume;
    cum_v_   += volume;
    cum_p2v_ += price * price * volume;
    ++sample_count_;
}

AnchoredVwapResult AnchoredVwap::snapshot(double current_price) const {
    AnchoredVwapResult r;
    r.anchor_ts_ns = anchor_ts_ns_;
    r.sample_count = sample_count_;
    if (cum_v_ <= 0.0 || sample_count_ < 5) return r;

    r.vwap = cum_pv_ / cum_v_;
    // Volume-weighted variance: E[p²] - (E[p])²
    double e_p2 = cum_p2v_ / cum_v_;
    double variance = std::max(0.0, e_p2 - r.vwap * r.vwap);
    double stddev = std::sqrt(variance);
    r.upper_1sigma = r.vwap + stddev;
    r.lower_1sigma = r.vwap - stddev;
    r.upper_2sigma = r.vwap + 2.0 * stddev;
    r.lower_2sigma = r.vwap - 2.0 * stddev;
    if (current_price > 0.0 && r.vwap > 0.0) {
        r.price_vs_vwap_bps = (current_price - r.vwap) / r.vwap * 10000.0;
    }
    r.valid = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// CvdTracker
// ─────────────────────────────────────────────────────────────────────────────

CvdTracker::CvdTracker(int recent_window) : recent_window_(recent_window) {}

void CvdTracker::reset() {
    samples_.clear();
    cum_cvd_ = 0.0;
    cum_volume_ = 0.0;
}

void CvdTracker::on_trade(double price, double volume, bool taker_buy, int64_t ts_ns) {
    if (!(volume > 0.0) || !std::isfinite(volume) || !std::isfinite(price)) return;
    double delta = taker_buy ? volume : -volume;
    cum_cvd_ += delta;
    cum_volume_ += volume;
    samples_.push_back({ts_ns, price, delta});
    if (static_cast<int>(samples_.size()) > recent_window_) {
        samples_.pop_front();
    }
}

CvdResult CvdTracker::snapshot() const {
    CvdResult r;
    r.sample_count = static_cast<int>(samples_.size());
    if (r.sample_count < 5) return r;
    r.cvd = cum_cvd_;
    r.cvd_normalized = (cum_volume_ > 0.0)
        ? std::clamp(cum_cvd_ / cum_volume_, -1.0, 1.0) : 0.0;

    // CVD change за recent_window — sum of deltas в окне.
    double recent_delta = 0.0;
    for (const auto& s : samples_) recent_delta += s.delta;
    r.cvd_change_recent = recent_delta;

    // Divergence: split window into 2 halves, compare CVD trend и price trend.
    if (r.sample_count >= 10) {
        size_t mid = samples_.size() / 2;
        double p_first_high = samples_[0].price, p_first_low = samples_[0].price;
        double p_last_high = samples_[mid].price, p_last_low = samples_[mid].price;
        double cvd_first = 0.0, cvd_last = 0.0;
        for (size_t i = 0; i < mid; ++i) {
            p_first_high = std::max(p_first_high, samples_[i].price);
            p_first_low = std::min(p_first_low, samples_[i].price);
            cvd_first += samples_[i].delta;
        }
        for (size_t i = mid; i < samples_.size(); ++i) {
            p_last_high = std::max(p_last_high, samples_[i].price);
            p_last_low = std::min(p_last_low, samples_[i].price);
            cvd_last += samples_[i].delta;
        }
        bool price_lower_lows = (p_last_low < p_first_low);
        bool price_higher_highs = (p_last_high > p_first_high);
        bool cvd_rising = (cvd_last > cvd_first);
        bool cvd_falling = (cvd_last < cvd_first);

        // Bullish divergence: price LL but CVD HL → buyers accumulating
        r.bullish_divergence = price_lower_lows && cvd_rising;
        // Bearish divergence: price HH but CVD falling → distribution
        r.bearish_divergence = price_higher_highs && cvd_falling;
    }
    r.valid = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// OiTracker
// ─────────────────────────────────────────────────────────────────────────────

OiTracker::OiTracker(int window) : window_(window) {}

void OiTracker::on_oi_update(double oi_usdt, double current_price, int64_t ts_ns) {
    if (!(oi_usdt > 0.0) || !std::isfinite(oi_usdt)) return;
    samples_.push_back({oi_usdt, current_price, ts_ns});
    if (static_cast<int>(samples_.size()) > window_) {
        samples_.pop_front();
    }
}

OiResult OiTracker::snapshot() const {
    OiResult r;
    r.sample_count = static_cast<int>(samples_.size());
    if (r.sample_count < 2) return r;

    r.oi_current = samples_.back().oi;
    double oi_first = samples_.front().oi;
    if (oi_first > 0.0) {
        r.oi_change_recent_pct = (r.oi_current - oi_first) / oi_first * 100.0;
    }
    // Trend quadrant (Wyckoff/COT):
    //   OI ↑ + Px ↑ = 1 (new longs, trend healthy)
    //   OI ↑ + Px ↓ = 2 (new shorts, trend healthy down)
    //   OI ↓ + Px ↑ = 3 (shorts covering, rally weak)
    //   OI ↓ + Px ↓ = 4 (longs liquidating, drop weak)
    bool oi_up = r.oi_change_recent_pct > 0.5;  // meaningful Δ
    bool oi_down = r.oi_change_recent_pct < -0.5;
    bool px_up = samples_.back().price > samples_.front().price;
    if (oi_up && px_up) r.trend_quadrant = 1;
    else if (oi_up && !px_up) r.trend_quadrant = 2;
    else if (oi_down && px_up) r.trend_quadrant = 3;
    else if (oi_down && !px_up) r.trend_quadrant = 4;
    r.valid = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// detect_liquidity_sweep
// ─────────────────────────────────────────────────────────────────────────────

LiquiditySweepResult detect_liquidity_sweep(
    const std::vector<double>& highs,
    const std::vector<double>& lows,
    const std::vector<double>& closes,
    int lookback,
    double wick_ratio_threshold,
    double recovery_threshold)
{
    LiquiditySweepResult r;
    const int n = static_cast<int>(closes.size());
    if (n < lookback + 1) return r;
    if (highs.size() != closes.size() || lows.size() != closes.size()) return r;

    // Recent N (excluding current bar) max/min.
    double recent_high = highs[n - lookback - 1];
    double recent_low = lows[n - lookback - 1];
    for (int i = n - lookback; i < n - 1; ++i) {
        recent_high = std::max(recent_high, highs[i]);
        recent_low = std::min(recent_low, lows[i]);
    }

    // Current bar.
    double cur_high = highs[n - 1];
    double cur_low = lows[n - 1];
    double cur_close = closes[n - 1];
    double bar_range = cur_high - cur_low;
    if (bar_range <= 0.0) return r;

    // Upper wick sweep: cur_high > recent_high AND close back below recent_high.
    if (cur_high > recent_high && cur_close < recent_high) {
        double upper_wick = cur_high - std::max(cur_close, cur_low);
        double wick_ratio = upper_wick / bar_range;
        double recovery = (cur_high - cur_close) / (cur_high - cur_low);
        if (wick_ratio >= wick_ratio_threshold && recovery >= recovery_threshold) {
            r.sweep_high_detected = true;
            r.sweep_price = cur_high;
            r.recovery_pct = recovery * 100.0;
        }
    }
    // Lower wick sweep: cur_low < recent_low AND close back above recent_low.
    if (cur_low < recent_low && cur_close > recent_low) {
        double lower_wick = std::min(cur_close, cur_high) - cur_low;
        double wick_ratio = lower_wick / bar_range;
        double recovery = (cur_close - cur_low) / (cur_high - cur_low);
        if (wick_ratio >= wick_ratio_threshold && recovery >= recovery_threshold) {
            r.sweep_low_detected = true;
            r.sweep_price = cur_low;
            r.recovery_pct = recovery * 100.0;
        }
    }
    r.bars_since_sweep = (r.sweep_high_detected || r.sweep_low_detected) ? 0 : -1;
    r.valid = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_queue_position
// ─────────────────────────────────────────────────────────────────────────────

QueuePositionResult estimate_queue_position(
    double our_order_usdt,
    double best_level_size_usdt,
    double queue_depletion_usdt_per_sec,
    int seconds_horizon)
{
    QueuePositionResult r;
    if (best_level_size_usdt <= 0.0 || our_order_usdt <= 0.0) return r;
    r.queue_size_usdt = best_level_size_usdt;
    r.our_order_usdt = our_order_usdt;

    // Estimated position — if we just placed: at back (position = 1).
    // For sustained scalp it's avg ~0.5 (FIFO assumption).
    // Conservative: estimate as our_order / queue_size (relative).
    double rel_position = std::clamp(our_order_usdt / best_level_size_usdt, 0.0, 1.0);
    r.estimated_position = std::clamp(0.5 + 0.5 * rel_position, 0.0, 1.0);

    // P(fill 30s): based на depletion rate. Cont-Larrard:
    //   P(fill) = P(volume executed > our_position_in_queue в seconds_horizon)
    if (queue_depletion_usdt_per_sec > 0.0) {
        double expected_depletion = queue_depletion_usdt_per_sec * seconds_horizon;
        double position_value = r.estimated_position * best_level_size_usdt + our_order_usdt;
        r.p_fill_30s = std::clamp(expected_depletion / position_value, 0.0, 1.0);
    }
    r.valid = true;
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// detect_spoofing
// ─────────────────────────────────────────────────────────────────────────────

SpoofResult detect_spoofing(
    double bid_depth_5_notional,
    double ask_depth_5_notional,
    double top_bid_size_notional,
    double top_ask_size_notional,
    double cancel_burst_intensity,
    double refill_asymmetry,
    double wall_threshold_pct,
    double burst_threshold)
{
    SpoofResult r;
    r.valid = true;
    int walls = 0;

    // Wall на bid: top bid > X% of total bid depth.
    if (bid_depth_5_notional > 0.0) {
        double bid_concentration = top_bid_size_notional / bid_depth_5_notional;
        if (bid_concentration > wall_threshold_pct) ++walls;
    }
    if (ask_depth_5_notional > 0.0) {
        double ask_concentration = top_ask_size_notional / ask_depth_5_notional;
        if (ask_concentration > wall_threshold_pct) ++walls;
    }
    r.suspicious_walls = walls;

    // Spoof bid: bid wall + active cancel burst + refill asymmetric AWAY from bid (negative).
    bool bid_wall = (bid_depth_5_notional > 0 && top_bid_size_notional / std::max(bid_depth_5_notional, 1e-12) > wall_threshold_pct);
    bool ask_wall = (ask_depth_5_notional > 0 && top_ask_size_notional / std::max(ask_depth_5_notional, 1e-12) > wall_threshold_pct);
    bool burst_active = cancel_burst_intensity > burst_threshold;

    // bid spoof: показные buys, на самом деле smart money will pull
    // → refill_asymmetry < -0.3 (more cancel on bid than ask)
    if (bid_wall && burst_active && refill_asymmetry < -0.3) {
        r.spoof_bid_detected = true;
    }
    if (ask_wall && burst_active && refill_asymmetry > 0.3) {
        r.spoof_ask_detected = true;
    }
    // Intensity: max of (cancel rate, wall concentration, asymmetry abs).
    r.spoof_intensity = std::clamp(
        std::max({cancel_burst_intensity,
                  bid_wall || ask_wall ? 0.5 : 0.0,
                  std::abs(refill_asymmetry)}),
        0.0, 1.0);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_liquidation_clusters
// ─────────────────────────────────────────────────────────────────────────────

LiquidationProxyResult estimate_liquidation_clusters(
    double oi_change_recent_pct,
    double funding_rate_8h,
    double price_momentum_recent,
    double current_leverage_assumption)
{
    LiquidationProxyResult r;
    r.valid = true;

    // Liquidation distance approx: 1 / leverage for isolated margin.
    // E.g. 10× leverage → ~10% adverse move = liq.
    double liq_distance_pct = 100.0 / std::max(current_leverage_assumption, 1.0);

    // Positive momentum + OI up = new longs at risk if reverse.
    // → upside liq cluster далеко, downside liq cluster close.
    bool longs_dominant = (oi_change_recent_pct > 1.0 && price_momentum_recent > 0.0) ||
                          (funding_rate_8h > 0.0005);
    bool shorts_dominant = (oi_change_recent_pct > 1.0 && price_momentum_recent < 0.0) ||
                           (funding_rate_8h < -0.0005);

    if (longs_dominant) {
        r.dominant_side = 1;  // longs at risk (drop = cascade liq)
        r.downside_liq_cluster_pct = liq_distance_pct * 0.7;  // typical cluster nearby
        r.upside_liq_cluster_pct = liq_distance_pct * 2.0;
    } else if (shorts_dominant) {
        r.dominant_side = -1;
        r.upside_liq_cluster_pct = liq_distance_pct * 0.7;
        r.downside_liq_cluster_pct = liq_distance_pct * 2.0;
    } else {
        r.upside_liq_cluster_pct = liq_distance_pct;
        r.downside_liq_cluster_pct = liq_distance_pct;
    }

    // Cascade risk: combo OI growth + funding extreme + momentum.
    // Bug 5.4 fix: 0.001 — funding extreme threshold, shared with evaluate_funding_bias().
    constexpr double kFundingExtremeThreshold = 0.001;
    double oi_factor = std::clamp(std::abs(oi_change_recent_pct) / 10.0, 0.0, 1.0);
    double funding_factor = std::clamp(std::abs(funding_rate_8h) / kFundingExtremeThreshold, 0.0, 1.0);
    double momentum_factor = std::clamp(std::abs(price_momentum_recent) / 0.02, 0.0, 1.0);
    r.cascade_risk_score = std::clamp(
        0.4 * oi_factor + 0.3 * funding_factor + 0.3 * momentum_factor, 0.0, 1.0);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// evaluate_funding_bias
// ─────────────────────────────────────────────────────────────────────────────

FundingBiasResult evaluate_funding_bias(double funding_rate_8h, double extreme_threshold) {
    FundingBiasResult r;
    if (!std::isfinite(funding_rate_8h)) return r;
    r.valid = true;
    r.funding_rate = funding_rate_8h;
    if (funding_rate_8h > extreme_threshold) {
        r.crowding_side = 1;  // longs crowded
        r.recommended_bias = -1;  // mean revert → short edge
    } else if (funding_rate_8h < -extreme_threshold) {
        r.crowding_side = -1;
        r.recommended_bias = 1;  // mean revert → long edge
    }
    r.crowding_intensity = std::clamp(
        std::abs(funding_rate_8h) / (extreme_threshold * 2.0), 0.0, 1.0);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_adaptive_thresholds
// ─────────────────────────────────────────────────────────────────────────────

AdaptiveThresholds compute_adaptive_thresholds(double realized_vol_5, double baseline_vol) {
    AdaptiveThresholds t;
    if (baseline_vol <= 0.0) return t;
    double vol_ratio = realized_vol_5 / baseline_vol;
    // Cap [0.5, 2.0] — даже extreme vol не открывает thresholds бесконечно.
    vol_ratio = std::clamp(vol_ratio, 0.5, 2.0);

    // High vol → wider RSI thresholds (нужны более extreme oscillator values).
    // E.g. vol_ratio=2 → RSI overbought = 75, oversold = 25.
    double rsi_offset = 20.0 * (vol_ratio - 1.0);  // ±5 to ±20
    t.rsi_overbought = std::clamp(70.0 + rsi_offset, 65.0, 85.0);
    t.rsi_oversold = std::clamp(30.0 - rsi_offset, 15.0, 35.0);

    // Momentum thresholds scale with vol.
    t.momentum_strong_buy = 0.005 * vol_ratio;
    t.momentum_strong_sell = -0.005 * vol_ratio;

    // BB breakout: tighten при low vol (компрессия = setup), widen при high.
    t.bb_breakout_pct_b = std::clamp(0.95 + 0.05 * (vol_ratio - 1.0), 0.90, 1.05);
    // B6.7 fix: %B по определению ≥ 0, нижняя граница 0.0.
    t.bb_breakdown_pct_b = std::clamp(0.05 - 0.05 * (vol_ratio - 1.0), 0.0, 0.10);
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// combine_signals_bayesian
// ─────────────────────────────────────────────────────────────────────────────

BayesianSignalScore combine_signals_bayesian(
    const std::vector<SignalLikelihood>& signals,
    double prior_bullish)
{
    BayesianSignalScore r;
    if (signals.empty()) {
        r.p_bullish = prior_bullish;
        r.p_bearish = 1.0 - prior_bullish;
        return r;
    }

    // B6.2 fix: clamp prior_bullish заранее чтобы prior_odds не уходил в 10^9.
    prior_bullish = std::clamp(prior_bullish, 0.01, 0.99);
    // Bayes: P(H|D) ∝ P(D|H) × P(H).
    double prior_odds = prior_bullish / std::max(1.0 - prior_bullish, 1e-9);
    double posterior_odds = prior_odds;
    for (const auto& s : signals) {
        if (s.lr_bullish <= 0.0 || s.lr_bearish <= 0.0) continue;
        // LR = P(D|H_bull) / P(D|H_bear). Каждый indicator передаёт уже эти likelihoods.
        double lr = s.lr_bullish / s.lr_bearish;
        // Cap для stability — extreme LRs не доминируют.
        lr = std::clamp(lr, 0.05, 20.0);
        posterior_odds *= lr;
    }
    // Convert back to probability.
    r.p_bullish = std::clamp(posterior_odds / (1.0 + posterior_odds), 0.01, 0.99);
    r.p_bearish = 1.0 - r.p_bullish;
    // Confidence: distance from 0.5.
    r.confidence = std::clamp(1.0 - 2.0 * std::min(r.p_bullish, r.p_bearish), 0.0, 1.0);
    if (r.p_bullish > 0.55) r.dominant_direction = 1;
    else if (r.p_bearish > 0.55) r.dominant_direction = -1;
    return r;
}

} // namespace tb::indicators
