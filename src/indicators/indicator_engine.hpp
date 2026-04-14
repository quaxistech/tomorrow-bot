#pragma once
#include "indicator_types.hpp"
#include "common/numeric_utils.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <vector>
#include <string>

namespace tb::indicators {

// Indicator engine — computes technical indicators using built-in implementations.
//
// All default parameter values follow canonical academic specifications:
//   SMA/BB(20)  — Bollinger (2002), "Bollinger on Bollinger Bands"
//   EMA(any)    — standard exponential smoothing, α = 2/(period+1)
//   RSI(14)     — Wilder (1978), "New Concepts in Technical Trading Systems"
//   MACD(12,26,9) — Appel (1979), original specification
//   ATR(14)     — Wilder (1978)
//   ADX(14)     — Wilder (1978)
//   BB stddev=2.0 — Bollinger (2002), captures ~95% of price action
class IndicatorEngine {
public:
    explicit IndicatorEngine(std::shared_ptr<tb::logging::ILogger> logger);

    // --- Core indicators (original API) ------------------------------------

    IndicatorResult sma(const std::vector<double>& prices, int period) const;
    IndicatorResult ema(const std::vector<double>& prices, int period) const;

    AdxResult adx(const std::vector<double>& high,
                  const std::vector<double>& low,
                  const std::vector<double>& close,
                  int period = 14) const;

    IndicatorResult atr(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        int period = 14) const;

    BollingerResult bollinger(const std::vector<double>& prices,
                              int period = 20,
                              double stddev = 2.0) const;

    IndicatorResult rsi(const std::vector<double>& prices, int period = 14) const;

    MacdResult macd(const std::vector<double>& prices,
                    int fast_period = 12,
                    int slow_period = 26,
                    int signal_period = 9) const;

    IndicatorResult obv(const std::vector<double>& prices,
                        const std::vector<double>& volumes) const;

    /// VWAP (Volume-Weighted Average Price) over last `window` bars.
    /// Default window=50 ≈ 1 hour of 1-minute bars; standard intraday
    /// anchor for mean-reversion scalping (Dacorogna et al., 2001).
    /// Returns scalar VWAP value; for band computation see rolling_vwap().
    IndicatorResult vwap(const std::vector<double>& high,
                         const std::vector<double>& low,
                         const std::vector<double>& close,
                         const std::vector<double>& volume,
                         int window = 50) const;

    // --- Extended indicators -----------------------------------------------

    /// Rolling VWAP with upper/lower bands (± band_stddev σ).
    /// Default band_stddev=1.0 — 1σ bands used in institutional execution
    /// for identifying mean-reversion zones (Kissell, 2014).
    VwapResult rolling_vwap(const std::vector<double>& high,
                            const std::vector<double>& low,
                            const std::vector<double>& close,
                            const std::vector<double>& volume,
                            int window = 50,
                            double band_stddev = 1.0) const;

    /// Rate of Change: ((price / price_n_ago) - 1) * 100.
    /// Default period=10 — standard ROC lookback (Pring, 2002,
    /// "Technical Analysis Explained").
    IndicatorResult rate_of_change(const std::vector<double>& prices,
                                   int period = 10) const;

    /// Z-Score of the latest price relative to the last `period` bars.
    /// Default period=20 — matches Bollinger Bands lookback (Bollinger, 2002).
    /// Uses population std (N divisor): the window IS the reference set,
    /// not a sample from a larger population.  Requires period >= 2.
    IndicatorResult z_score(const std::vector<double>& prices,
                            int period = 20) const;

    /// Realized volatility: sample std-dev of log-returns over last `period` bars.
    /// Requires period+1 prices.  Uses Bessel-corrected (n-1) estimator.
    /// Default period=20 ≈ 1 trading session on 1m charts.
    /// Reference: Hull (2018), "Options, Futures, and Other Derivatives".
    IndicatorResult volatility(const std::vector<double>& prices,
                               int period = 20) const;

    /// Price momentum: (close_now − close_n_ago) / close_n_ago as a fraction.
    /// Requires period+1 prices.  Default period=20 — consistent with
    /// volatility lookback for momentum/vol ratio analysis.
    IndicatorResult momentum(const std::vector<double>& prices,
                             int period = 20) const;

private:
    // Computes full EMA series; output[i] valid for i >= period-1
    std::vector<double> ema_series(const std::vector<double>& prices, int period) const;

    std::shared_ptr<tb::logging::ILogger> logger_;
};

} // namespace tb::indicators
