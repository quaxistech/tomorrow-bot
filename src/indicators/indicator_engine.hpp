#pragma once
#include "indicator_types.hpp"
#include "common/numeric_utils.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <vector>
#include <string>

namespace tb::indicators {

// Base indicator interface
class IIndicator {
public:
    virtual ~IIndicator() = default;
    virtual std::string name() const = 0;
    virtual int min_periods() const = 0;
};

// Indicator engine — computes technical indicators using built-in implementations.
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

    IndicatorResult vwap(const std::vector<double>& high,
                         const std::vector<double>& low,
                         const std::vector<double>& close,
                         const std::vector<double>& volume) const;

    // --- Extended indicators -----------------------------------------------

    /// Session-aware rolling VWAP with upper/lower bands (± band_stddev).
    VwapResult rolling_vwap(const std::vector<double>& high,
                            const std::vector<double>& low,
                            const std::vector<double>& close,
                            const std::vector<double>& volume,
                            int window = 50,
                            double band_stddev = 1.0) const;

    /// Rate of Change: ((price / price_n_ago) - 1) * 100
    IndicatorResult rate_of_change(const std::vector<double>& prices,
                                   int period = 10) const;

    /// Z-Score of the latest price relative to the last `period` bars.
    IndicatorResult z_score(const std::vector<double>& prices,
                            int period = 20) const;

private:
    // Computes full EMA series; output[i] valid for i >= period-1
    std::vector<double> ema_series(const std::vector<double>& prices, int period) const;

    IndicatorResult sma_builtin(const std::vector<double>& prices, int period) const;
    IndicatorResult ema_builtin(const std::vector<double>& prices, int period) const;
    IndicatorResult rsi_builtin(const std::vector<double>& prices, int period) const;
    MacdResult macd_builtin(const std::vector<double>& prices, int fast, int slow, int signal) const;
    BollingerResult bollinger_builtin(const std::vector<double>& prices, int period, double stddev) const;
    AdxResult adx_builtin(const std::vector<double>& high,
                          const std::vector<double>& low,
                          const std::vector<double>& close, int period) const;
    IndicatorResult atr_builtin(const std::vector<double>& high,
                                const std::vector<double>& low,
                                const std::vector<double>& close, int period) const;
    IndicatorResult obv_builtin(const std::vector<double>& prices,
                                const std::vector<double>& volumes) const;

    std::shared_ptr<tb::logging::ILogger> logger_;
};

} // namespace tb::indicators
