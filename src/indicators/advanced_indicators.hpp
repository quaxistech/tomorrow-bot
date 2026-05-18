#pragma once
/**
 * @file advanced_indicators.hpp
 * @brief Professional indicators для signal precision (run94 expansion).
 *
 * Покрывает:
 *   - Anchored VWAP (session/daily) + standard deviation bands
 *   - Cumulative Volume Delta (CVD) + delta divergence detection
 *   - Open Interest tracking + OI delta velocity
 *   - Liquidity Sweep Detector (wick reversal analysis)
 *   - Queue Position Estimator (orderbook depth dynamics)
 *   - Spoof Detector (top-of-book quote churn analysis)
 *   - Liquidation Heatmap (OI delta + leverage clustering proxy)
 *   - Funding Bias filter
 *   - Adaptive Thresholds (rolling-vol normalized RSI/momentum bounds)
 *   - Bayesian Signal Combiner (multi-indicator probability fusion)
 *
 * Все индикаторы — pure functions либо stateful streaming для эффективности.
 * Academic references — см. каждый struct/method.
 */

#include "indicator_types.hpp"
#include <vector>
#include <deque>
#include <cstdint>
#include <optional>

namespace tb::indicators {

// ─────────────────────────────────────────────────────────────────────────────
// Anchored VWAP
// ─────────────────────────────────────────────────────────────────────────────

/// Anchored VWAP (Carter "Mastering the Trade" 2012). VWAP с привязкой к
/// anchor timestamp (обычно session start = UTC 00:00). Дает "fair price"
/// для текущей сессии. Pivots от него = high-probability levels.
struct AnchoredVwapResult {
    bool valid{false};
    double vwap{0.0};                ///< Текущий anchored VWAP
    double upper_1sigma{0.0};        ///< +1σ band (volume-weighted stddev)
    double lower_1sigma{0.0};
    double upper_2sigma{0.0};        ///< +2σ band — entry/stop levels
    double lower_2sigma{0.0};
    double price_vs_vwap_bps{0.0};   ///< (price - vwap) / vwap × 10000
    int64_t anchor_ts_ns{0};         ///< Timestamp начала сессии
    int sample_count{0};
};

/// Stateful Anchored VWAP — поддерживает streaming с reset по anchor.
class AnchoredVwap {
public:
    /// session_window_ms: UTC time-of-day reset для daily anchor (default 24h).
    /// Если хотим intra-day session (Asia/EU/US) — устанавливаем меньше.
    explicit AnchoredVwap(int64_t session_window_ns = 24LL * 3600 * 1'000'000'000LL);

    /// Update on each trade. price = exec price, volume = quote volume в USDT.
    /// ts_ns — trade timestamp. Если ts_ns пересёк новый anchor period — reset.
    void on_trade(double price, double volume, int64_t ts_ns);

    /// Принудительный reset (для anchor change или session pivot).
    void reset(int64_t new_anchor_ts_ns);

    [[nodiscard]] AnchoredVwapResult snapshot(double current_price) const;

private:
    int64_t session_window_ns_;
    int64_t anchor_ts_ns_{0};
    double cum_pv_{0.0};          ///< Σ price × volume
    double cum_v_{0.0};           ///< Σ volume
    double cum_p2v_{0.0};         ///< Σ price² × volume (для variance)
    int sample_count_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Cumulative Volume Delta + Divergence
// ─────────────────────────────────────────────────────────────────────────────

/// CVD (Cumulative Volume Delta): рост buy_volume - sell_volume.
/// Показывает накопление позиций (taker buy vs taker sell pressure).
/// Divergence cvd vs price = leading reversal signal.
struct CvdResult {
    bool valid{false};
    double cvd{0.0};                     ///< Cumulative delta (текущий)
    double cvd_change_recent{0.0};       ///< Δ CVD за последние N samples
    double cvd_normalized{0.0};          ///< CVD / total_volume_window — [-1..+1]
    bool bullish_divergence{false};      ///< Price LL but CVD HL (buying despite drop)
    bool bearish_divergence{false};      ///< Price HH but CVD LH (selling into rally)
    int sample_count{0};
};

/// Stateful CVD tracker — streaming на trade events.
class CvdTracker {
public:
    explicit CvdTracker(int recent_window = 60);

    /// taker_aggressor: true = buy aggressor (taker buy), false = sell aggressor.
    void on_trade(double price, double volume, bool taker_buy, int64_t ts_ns);

    /// Reset (e.g. on session anchor change).
    void reset();

    [[nodiscard]] CvdResult snapshot() const;

private:
    struct Sample {
        int64_t ts_ns;
        double price;
        double delta;       ///< +volume if buy, -volume if sell
    };
    int recent_window_;
    std::deque<Sample> samples_;
    double cum_cvd_{0.0};
    double cum_volume_{0.0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Open Interest Tracking
// ─────────────────────────────────────────────────────────────────────────────

/// OI evolution + velocity. OI ↑ + price ↑ = new longs (trend continuation).
/// OI ↓ + price ↑ = shorts covering (rally weak). И т.д. — 4 квадранта Wyckoff.
struct OiResult {
    bool valid{false};
    double oi_current{0.0};              ///< Текущий OI (notional USDT)
    double oi_change_recent_pct{0.0};    ///< % change за last_n samples
    int trend_quadrant{0};               ///< 1=OI↑+Px↑, 2=OI↑+Px↓, 3=OI↓+Px↑, 4=OI↓+Px↓
    int sample_count{0};
};

class OiTracker {
public:
    explicit OiTracker(int window = 20);

    /// Update from periodic REST poll.
    void on_oi_update(double oi_usdt, double current_price, int64_t ts_ns);

    [[nodiscard]] OiResult snapshot() const;

private:
    struct Sample { double oi; double price; int64_t ts_ns; };
    int window_;
    std::deque<Sample> samples_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Liquidity Sweep Detector
// ─────────────────────────────────────────────────────────────────────────────

/// Liquidity Sweep (smart-money concept): wick beyond local high/low followed
/// by revert. Сигнализирует stop-hunt + reversal opportunity.
/// При detection — НЕ открывать в направлении wick (likely fake).
struct LiquiditySweepResult {
    bool valid{false};
    bool sweep_high_detected{false};     ///< Wicked above recent high, closed back
    bool sweep_low_detected{false};      ///< Wicked below recent low, closed back
    double sweep_price{0.0};             ///< Цена sweep extreme
    double recovery_pct{0.0};            ///< % восстановления от wick к close
    int bars_since_sweep{0};
};

/// candles_high/low/close — последние N свечей (например 20 1m).
/// wick_ratio_threshold — минимальный (wick / total_range) для detection.
/// recovery_threshold — close должен восстановиться на ≥ X% от wick.
LiquiditySweepResult detect_liquidity_sweep(
    const std::vector<double>& highs,
    const std::vector<double>& lows,
    const std::vector<double>& closes,
    int lookback = 10,
    double wick_ratio_threshold = 0.5,
    double recovery_threshold = 0.6);

// ─────────────────────────────────────────────────────────────────────────────
// Queue Position Estimator
// ─────────────────────────────────────────────────────────────────────────────

/// Queue Position (Cont, Larrard 2013). Estimate где наш limit order стоит
/// в очереди на best bid/ask. Используется для решения post-only vs market.
/// Высокая позиция (front of queue) → высокая P(fill).
struct QueuePositionResult {
    bool valid{false};
    /// Estimated position в очереди: 0 = front (наш ордер первый), 1 = back.
    double estimated_position{0.5};
    double queue_size_usdt{0.0};         ///< Total queue value at our level
    double our_order_usdt{0.0};
    /// Probability fill в next X seconds (based on queue depletion rate).
    double p_fill_30s{0.0};
};

/// Estimate based на orderbook depth и наш order size+price.
/// queue_depletion_bps_per_sec — recent rate at which best level shrinks.
QueuePositionResult estimate_queue_position(
    double our_order_usdt,
    double best_level_size_usdt,
    double queue_depletion_usdt_per_sec,
    int seconds_horizon = 30);

// ─────────────────────────────────────────────────────────────────────────────
// Spoof Detector (enhanced)
// ─────────────────────────────────────────────────────────────────────────────

/// Spoof detection (Cartea, Jaimungal 2015). Большие limit orders которые
/// быстро cancel'ятся = манипуляция. Признаки:
///   1. Wall > X% от total depth на одной стороне
///   2. Cancel rate > Y per second на best levels
///   3. Asymmetric depth (bid >> ask) с быстрой rebalance
struct SpoofResult {
    bool valid{false};
    bool spoof_bid_detected{false};      ///< Spoofing на bid (fake demand)
    bool spoof_ask_detected{false};      ///< Spoofing на ask (fake supply)
    double spoof_intensity{0.0};         ///< 0..1, сила сигнала
    int suspicious_walls{0};             ///< Count walls > threshold
};

SpoofResult detect_spoofing(
    double bid_depth_5_notional,
    double ask_depth_5_notional,
    double top_bid_size_notional,
    double top_ask_size_notional,
    double cancel_burst_intensity,
    double refill_asymmetry,
    double wall_threshold_pct = 0.30,
    double burst_threshold = 0.50);

// ─────────────────────────────────────────────────────────────────────────────
// Liquidation Heatmap (Proxy)
// ─────────────────────────────────────────────────────────────────────────────

/// Liquidation Heatmap proxy. Реальная heatmap требует liquidation feed.
/// Этот proxy estimate'ит likely liquidation clusters через:
///   - OI delta velocity (новые позиции = новые liquidation levels)
///   - Funding extremes (skewed positioning)
///   - Price runup velocity (cascades probability)
struct LiquidationProxyResult {
    bool valid{false};
    double upside_liq_cluster_pct{0.0};  ///< % выше price где likely longs liq'd
    double downside_liq_cluster_pct{0.0};///< % ниже price где likely shorts liq'd
    double cascade_risk_score{0.0};      ///< 0..1, риск cascade liquidation
    int dominant_side{0};                ///< +1 longs at risk, -1 shorts at risk
};

LiquidationProxyResult estimate_liquidation_clusters(
    double oi_change_recent_pct,
    double funding_rate_8h,
    double price_momentum_recent,
    double current_leverage_assumption = 10.0);

// ─────────────────────────────────────────────────────────────────────────────
// Funding Bias
// ─────────────────────────────────────────────────────────────────────────────

/// Funding bias signal. Funding rate показывает positioning crowding.
///   - Positive funding extreme (longs pay shorts): long crowded → mean revert short edge
///   - Negative funding extreme: short crowded → mean revert long edge
struct FundingBiasResult {
    bool valid{false};
    double funding_rate{0.0};
    int crowding_side{0};                ///< +1 longs crowded, -1 shorts crowded
    double crowding_intensity{0.0};      ///< 0..1
    int recommended_bias{0};             ///< +1 long edge, -1 short edge (от mean revert)
};

FundingBiasResult evaluate_funding_bias(double funding_rate_8h,
                                          double extreme_threshold = 0.001);

// ─────────────────────────────────────────────────────────────────────────────
// Adaptive Thresholds
// ─────────────────────────────────────────────────────────────────────────────

/// Adaptive RSI/momentum thresholds based on rolling volatility.
/// Higher vol → wider thresholds (более экстремальные значения нужны для signal).
struct AdaptiveThresholds {
    double rsi_overbought{70.0};
    double rsi_oversold{30.0};
    double momentum_strong_buy{0.005};
    double momentum_strong_sell{-0.005};
    double bb_breakout_pct_b{0.95};
    double bb_breakdown_pct_b{0.05};
};

/// realized_vol_5: std-dev log returns за последние 5 баров (1m=5min).
/// base_vol: baseline vol для нормализации (typically median historical vol).
AdaptiveThresholds compute_adaptive_thresholds(double realized_vol_5,
                                                 double baseline_vol = 0.003);

// ─────────────────────────────────────────────────────────────────────────────
// Bayesian Signal Combiner
// ─────────────────────────────────────────────────────────────────────────────

/// Combine multi-indicator signals в единый probability score.
/// P(direction) = bayesian update от each indicator's likelihood ratio.
struct BayesianSignalScore {
    double p_bullish{0.5};               ///< Posterior P(bullish move next N bars)
    double p_bearish{0.5};
    double confidence{0.0};              ///< 1 - 2 × min(p_bull, p_bear) — далеко от 0.5 = high conf
    int dominant_direction{0};           ///< +1 bullish, -1 bearish, 0 neutral
};

/// Bayesian fusion of signal likelihoods.
/// Each signal = (likelihood_if_bullish, likelihood_if_bearish).
/// Combined: P(bullish | data) = product of likelihood_ratios normalized.
struct SignalLikelihood {
    double lr_bullish;       ///< Likelihood ratio if bullish (>1 favors bullish)
    double lr_bearish;       ///< Likelihood ratio if bearish (>1 favors bearish)
};

BayesianSignalScore combine_signals_bayesian(
    const std::vector<SignalLikelihood>& signals,
    double prior_bullish = 0.5);

} // namespace tb::indicators
