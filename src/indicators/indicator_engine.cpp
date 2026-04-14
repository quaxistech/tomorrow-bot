#include "indicator_engine.hpp"
#include "common/numeric_utils.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace tb::indicators {

using numeric::safe_div;
using numeric::safe_sqrt;
using numeric::validate_price_series;
using numeric::validate_series_alignment;
using numeric::is_valid_price;
using numeric::is_valid_volume;
using numeric::kEpsilon;
using numeric::kMinVariance;

// ─────────────────────────────────────────────────────────────────────────────
// Construction & utilities
// ─────────────────────────────────────────────────────────────────────────────

IndicatorEngine::IndicatorEngine(std::shared_ptr<tb::logging::ILogger> logger)
    : logger_(std::move(logger))
{
    if (logger_) {
        logger_->info("IndicatorEngine", "Initialized with built-in indicator implementations");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: full EMA series
// output[i] valid for i >= period-1; earlier entries are 0.0
// ─────────────────────────────────────────────────────────────────────────────

std::vector<double> IndicatorEngine::ema_series(const std::vector<double>& prices, int period) const {
    const std::size_t n = prices.size();
    std::vector<double> out(n, 0.0);
    if (n < static_cast<std::size_t>(period) || period <= 0) return out;

    double seed = 0.0;
    for (int i = 0; i < period; ++i) seed += prices[i];
    seed /= static_cast<double>(period);
    out[period - 1] = seed;

    const double k = 2.0 / (static_cast<double>(period) + 1.0);
    for (std::size_t i = static_cast<std::size_t>(period); i < n; ++i) {
        out[i] = prices[i] * k + out[i - 1] * (1.0 - k);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// SMA
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::sma(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "SMA";
    const auto n = static_cast<int>(prices.size());

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "SMA: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (n < period) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = period - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "SMA: invalid price data detected");
        return result;
    }

    double sum = 0.0;
    const std::size_t start = prices.size() - static_cast<std::size_t>(period);
    for (std::size_t i = start; i < prices.size(); ++i) {
        sum += prices[i];
    }

    result.valid = true;
    result.value = safe_div(sum, static_cast<double>(period));
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// EMA
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::ema(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "EMA";
    const auto n = static_cast<int>(prices.size());

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "EMA: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (n < period) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = period - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "EMA: invalid price data detected");
        return result;
    }

    const auto series = ema_series(prices, period);

    result.valid = true;
    result.value = series.back();
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// RSI (Wilder's method)
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::rsi(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "RSI";
    const auto n = static_cast<int>(prices.size());
    const int min_bars = period + 1;

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "RSI: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "RSI: invalid price data detected");
        return result;
    }

    double avg_gain = 0.0, avg_loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        const double delta = prices[i] - prices[i - 1];
        if (delta > 0.0) avg_gain += delta;
        else             avg_loss += -delta;
    }
    avg_gain /= static_cast<double>(period);
    avg_loss /= static_cast<double>(period);

    for (std::size_t i = static_cast<std::size_t>(period) + 1; i < static_cast<std::size_t>(n); ++i) {
        const double delta = prices[i] - prices[i - 1];
        const double gain = delta > 0.0 ? delta : 0.0;
        const double loss = delta < 0.0 ? -delta : 0.0;
        avg_gain = (avg_gain * static_cast<double>(period - 1) + gain) / static_cast<double>(period);
        avg_loss = (avg_loss * static_cast<double>(period - 1) + loss) / static_cast<double>(period);
    }

    if (avg_loss < kEpsilon) {
        result.value = 100.0;
    } else {
        const double rs = safe_div(avg_gain, avg_loss);
        result.value = 100.0 - safe_div(100.0, 1.0 + rs);
    }

    result.valid = true;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// MACD
// ─────────────────────────────────────────────────────────────────────────────

MacdResult IndicatorEngine::macd(const std::vector<double>& prices,
                                  int fast, int slow, int signal) const {
    MacdResult result;
    const auto n = static_cast<int>(prices.size());
    const int min_bars = slow + signal - 1;

    if (fast <= 0 || slow <= 0 || signal <= 0 || fast >= slow) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "MACD: invalid parameters",
            {{"fast", std::to_string(fast)}, {"slow", std::to_string(slow)}, {"signal", std::to_string(signal)}});
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "MACD: invalid price data detected");
        return result;
    }

    const auto fast_ema  = ema_series(prices, fast);
    const auto slow_ema  = ema_series(prices, slow);

    const std::size_t macd_start = static_cast<std::size_t>(slow - 1);
    const std::size_t macd_len   = static_cast<std::size_t>(n) - macd_start;
    std::vector<double> macd_line(macd_len);
    for (std::size_t i = 0; i < macd_len; ++i) {
        macd_line[i] = fast_ema[macd_start + i] - slow_ema[macd_start + i];
    }

    if (macd_line.size() < static_cast<std::size_t>(signal)) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = signal - static_cast<int>(macd_line.size());
        return result;
    }

    const auto signal_series = ema_series(macd_line, signal);

    result.valid = true;
    result.macd = macd_line.back();
    result.signal = signal_series.back();
    result.histogram = result.macd - result.signal;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Bollinger Bands
// ─────────────────────────────────────────────────────────────────────────────

BollingerResult IndicatorEngine::bollinger(const std::vector<double>& prices,
                                            int period, double stddev_mult) const {
    BollingerResult result;
    const auto n = static_cast<int>(prices.size());

    if (period <= 0 || stddev_mult < 0.0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Bollinger: invalid parameters",
            {{"period", std::to_string(period)}});
        return result;
    }
    if (n < period) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = period - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Bollinger: invalid price data detected");
        return result;
    }

    const std::size_t start = prices.size() - static_cast<std::size_t>(period);

    double sum = 0.0;
    for (std::size_t i = start; i < prices.size(); ++i) sum += prices[i];
    const double middle = safe_div(sum, static_cast<double>(period));

    double var = 0.0;
    for (std::size_t i = start; i < prices.size(); ++i) {
        const double d = prices[i] - middle;
        var += d * d;
    }
    const double sigma = safe_sqrt(safe_div(var, static_cast<double>(period)));

    const double upper = middle + stddev_mult * sigma;
    const double lower = middle - stddev_mult * sigma;
    const double bandwidth = safe_div(upper - lower, middle);

    double percent_b = 0.5;
    const double band_width_raw = upper - lower;
    if (band_width_raw > kEpsilon) {
        percent_b = safe_div(prices.back() - lower, band_width_raw, 0.5);
    }

    result.valid = true;
    result.upper = upper;
    result.middle = middle;
    result.lower = lower;
    result.bandwidth = bandwidth;
    result.percent_b = percent_b;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// ATR (Average True Range, Wilder's method)
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::atr(const std::vector<double>& high,
                                      const std::vector<double>& low,
                                      const std::vector<double>& close,
                                      int period) const {
    IndicatorResult result;
    result.name = "ATR";
    const auto n = static_cast<int>(high.size());
    const int min_bars = period + 1;

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ATR: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (high.size() != low.size() || high.size() != close.size()) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ATR: series length mismatch");
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(high) || !validate_price_series(low) || !validate_price_series(close)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ATR: invalid price data detected");
        return result;
    }

    std::vector<double> tr(static_cast<std::size_t>(n) - 1);
    for (std::size_t i = 1; i < static_cast<std::size_t>(n); ++i) {
        const double hl  = high[i] - low[i];
        const double hpc = std::abs(high[i]  - close[i - 1]);
        const double lpc = std::abs(low[i]   - close[i - 1]);
        tr[i - 1] = std::max({hl, hpc, lpc});
    }

    double atr_val = 0.0;
    for (int i = 0; i < period; ++i) atr_val += tr[i];
    atr_val = safe_div(atr_val, static_cast<double>(period));

    const std::size_t tr_n = tr.size();
    for (std::size_t i = static_cast<std::size_t>(period); i < tr_n; ++i) {
        atr_val = safe_div(atr_val * static_cast<double>(period - 1) + tr[i],
                           static_cast<double>(period));
    }

    result.valid = true;
    result.value = atr_val;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// ADX (+DI, -DI, ADX)
// ─────────────────────────────────────────────────────────────────────────────

AdxResult IndicatorEngine::adx(const std::vector<double>& high,
                                 const std::vector<double>& low,
                                 const std::vector<double>& close,
                                 int period) const {
    AdxResult result;
    const auto n = static_cast<int>(high.size());
    const int min_bars = 2 * period + 1;

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ADX: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (high.size() != low.size() || high.size() != close.size()) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ADX: series length mismatch");
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(high) || !validate_price_series(low) || !validate_price_series(close)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ADX: invalid price data detected");
        return result;
    }

    const std::size_t bars = static_cast<std::size_t>(n) - 1;
    std::vector<double> plus_dm(bars), minus_dm(bars), tr_v(bars);
    for (std::size_t i = 1; i < static_cast<std::size_t>(n); ++i) {
        const double up   = high[i]  - high[i - 1];
        const double down = low[i - 1] - low[i];

        plus_dm[i - 1]  = (up > down && up > 0.0) ? up   : 0.0;
        minus_dm[i - 1] = (down > up && down > 0.0) ? down : 0.0;

        const double hl  = high[i] - low[i];
        const double hpc = std::abs(high[i]  - close[i - 1]);
        const double lpc = std::abs(low[i]   - close[i - 1]);
        tr_v[i - 1] = std::max({hl, hpc, lpc});
    }

    double smooth_pdm = 0.0, smooth_mdm = 0.0, smooth_tr = 0.0;
    for (int i = 0; i < period; ++i) {
        smooth_pdm += plus_dm[i];
        smooth_mdm += minus_dm[i];
        smooth_tr  += tr_v[i];
    }

    std::vector<double> dx_series;
    dx_series.reserve(bars - static_cast<std::size_t>(period));

    auto calc_dx = [&]() -> double {
        const double plus_di_val  = safe_div(100.0 * smooth_pdm, smooth_tr);
        const double minus_di_val = safe_div(100.0 * smooth_mdm, smooth_tr);
        const double di_sum = plus_di_val + minus_di_val;
        return safe_div(100.0 * std::abs(plus_di_val - minus_di_val), di_sum);
    };

    dx_series.push_back(calc_dx());

    for (std::size_t i = static_cast<std::size_t>(period); i < bars; ++i) {
        smooth_pdm = smooth_pdm - safe_div(smooth_pdm, static_cast<double>(period)) + plus_dm[i];
        smooth_mdm = smooth_mdm - safe_div(smooth_mdm, static_cast<double>(period)) + minus_dm[i];
        smooth_tr  = smooth_tr  - safe_div(smooth_tr,  static_cast<double>(period)) + tr_v[i];
        dx_series.push_back(calc_dx());
    }

    if (dx_series.size() < static_cast<std::size_t>(period)) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = period - static_cast<int>(dx_series.size());
        return result;
    }

    double adx_val = 0.0;
    for (int i = 0; i < period; ++i) adx_val += dx_series[i];
    adx_val = safe_div(adx_val, static_cast<double>(period));

    for (std::size_t i = static_cast<std::size_t>(period); i < dx_series.size(); ++i) {
        adx_val = safe_div(adx_val * static_cast<double>(period - 1) + dx_series[i],
                           static_cast<double>(period));
    }

    const double final_plus_di  = safe_div(100.0 * smooth_pdm, smooth_tr);
    const double final_minus_di = safe_div(100.0 * smooth_mdm, smooth_tr);

    result.valid = true;
    result.adx = adx_val;
    result.plus_di = final_plus_di;
    result.minus_di = final_minus_di;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// OBV (On-Balance Volume)
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::obv(const std::vector<double>& prices,
                                      const std::vector<double>& volumes) const {
    IndicatorResult result;
    result.name = "OBV";
    const auto n = static_cast<int>(prices.size());

    if (prices.size() != volumes.size()) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "OBV: prices/volumes length mismatch");
        return result;
    }
    if (n < 2) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = 2 - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "OBV: invalid price data detected");
        return result;
    }
    for (const auto& v : volumes) {
        if (!is_valid_volume(v)) {
            result.status = IndicatorStatus::InvalidInput;
            if (logger_) logger_->warn("IndicatorEngine", "OBV: non-finite or invalid volume detected");
            return result;
        }
    }

    double obv_val = 0.0;
    for (std::size_t i = 1; i < static_cast<std::size_t>(n); ++i) {
        if (prices[i] > prices[i - 1])      obv_val += volumes[i];
        else if (prices[i] < prices[i - 1]) obv_val -= volumes[i];
    }

    result.valid = true;
    result.value = obv_val;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// VWAP (scalar, delegates to rolling_vwap for DRY)
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::vwap(const std::vector<double>& high,
                                       const std::vector<double>& low,
                                       const std::vector<double>& close,
                                       const std::vector<double>& volume,
                                       int window) const {
    // Delegate to rolling_vwap with band_stddev=0 (bands not needed for scalar result).
    auto rv = rolling_vwap(high, low, close, volume, window, 0.0);

    IndicatorResult result;
    result.name = "VWAP";
    result.valid = rv.valid;
    result.value = rv.vwap;
    result.status = rv.status;
    result.sample_count = rv.sample_count;
    result.warmup_remaining = rv.warmup_remaining;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rolling VWAP with bands (± stddev)
// ─────────────────────────────────────────────────────────────────────────────

VwapResult IndicatorEngine::rolling_vwap(const std::vector<double>& high,
                                          const std::vector<double>& low,
                                          const std::vector<double>& close,
                                          const std::vector<double>& volume,
                                          int window, double band_stddev) const {
    VwapResult result;
    const auto n = static_cast<int>(high.size());

    if (window <= 0 || band_stddev < 0.0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "rolling_vwap: invalid parameters",
            {{"window", std::to_string(window)}});
        return result;
    }
    if (n == 0) {
        result.status = IndicatorStatus::InsufficientData;
        result.warmup_remaining = 1;
        return result;
    }
    if (high.size() != low.size() || high.size() != close.size() || high.size() != volume.size()) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "rolling_vwap: series length mismatch");
        return result;
    }
    if (!validate_price_series(high) || !validate_price_series(low) || !validate_price_series(close)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "rolling_vwap: invalid price data");
        return result;
    }
    for (const auto& v : volume) {
        if (!is_valid_volume(v)) {
            result.status = IndicatorStatus::InvalidInput;
            if (logger_) logger_->warn("IndicatorEngine", "rolling_vwap: invalid volume data");
            return result;
        }
    }

    const std::size_t eff_window = std::min(static_cast<std::size_t>(window),
                                            static_cast<std::size_t>(n));
    const std::size_t start = static_cast<std::size_t>(n) - eff_window;

    double sum_tpv = 0.0, sum_vol = 0.0;
    std::vector<double> typical_prices(eff_window);
    for (std::size_t i = 0; i < eff_window; ++i) {
        const std::size_t idx = start + i;
        const double tp = (high[idx] + low[idx] + close[idx]) / 3.0;
        typical_prices[i] = tp;
        sum_tpv += tp * volume[idx];
        sum_vol += volume[idx];
    }

    if (sum_vol < kEpsilon) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "rolling_vwap: zero total volume");
        return result;
    }

    const double vwap_val = safe_div(sum_tpv, sum_vol);

    // Volume-weighted variance for bands
    double weighted_var = 0.0;
    for (std::size_t i = 0; i < eff_window; ++i) {
        const std::size_t idx = start + i;
        const double dev = typical_prices[i] - vwap_val;
        weighted_var += volume[idx] * dev * dev;
    }
    const double stddev = safe_sqrt(safe_div(weighted_var, sum_vol));

    result.valid = true;
    result.vwap = vwap_val;
    result.upper_band = vwap_val + band_stddev * stddev;
    result.lower_band = vwap_val - band_stddev * stddev;
    result.cumulative_volume = sum_vol;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = std::max(0, window - n);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rate of Change: ((price / price_n_ago) - 1) * 100
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::rate_of_change(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "ROC";
    const auto n = static_cast<int>(prices.size());
    const int min_bars = period + 1;

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ROC: invalid period", {{"period", std::to_string(period)}});
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "ROC: invalid price data detected");
        return result;
    }

    const double current = prices.back();
    const double past    = prices[prices.size() - 1 - static_cast<std::size_t>(period)];

    result.valid = true;
    result.value = safe_div(current - past, past) * 100.0;
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Z-Score of latest price relative to the last `period` bars
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::z_score(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "ZSCORE";
    const auto n = static_cast<int>(prices.size());

    if (period < 2) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Z-Score: period must be >= 2", {{"period", std::to_string(period)}});
        return result;
    }
    if (n < period) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = period - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Z-Score: invalid price data detected");
        return result;
    }

    const std::size_t start = prices.size() - static_cast<std::size_t>(period);

    double sum = 0.0;
    for (std::size_t i = start; i < prices.size(); ++i) sum += prices[i];
    const double mean = safe_div(sum, static_cast<double>(period));

    double var = 0.0;
    for (std::size_t i = start; i < prices.size(); ++i) {
        const double d = prices[i] - mean;
        var += d * d;
    }
    const double stddev = safe_sqrt(safe_div(var, static_cast<double>(period)));

    result.valid = true;
    result.value = safe_div(prices.back() - mean, stddev);
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Realized Volatility (sample std-dev of log-returns)
//
// Reference: Hull (2018), "Options, Futures, and Other Derivatives".
// Log-returns are preferred over simple returns in financial econometrics
// because they are time-additive and approximately normally distributed.
// Bessel correction (n-1) gives an unbiased variance estimator.
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::volatility(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "VOLATILITY";
    const auto n = static_cast<int>(prices.size());
    const int min_bars = period + 1;  // need period+1 prices → period returns

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Volatility: invalid period",
            {{"period", std::to_string(period)}});
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Volatility: invalid price data detected");
        return result;
    }

    // Compute log-returns over the last `period` bars
    const std::size_t start = prices.size() - static_cast<std::size_t>(min_bars);
    double sum = 0.0;
    std::size_t count = 0;

    // First pass: compute mean of log-returns
    for (std::size_t i = start; i < prices.size() - 1; ++i) {
        if (prices[i] <= 0.0 || prices[i + 1] <= 0.0) continue;
        sum += std::log(prices[i + 1] / prices[i]);
        ++count;
    }

    if (count < 2) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = 2 - static_cast<int>(count);
        return result;
    }

    const double mean_ret = sum / static_cast<double>(count);

    // Second pass: compute variance with Bessel correction (n-1)
    double var = 0.0;
    for (std::size_t i = start; i < prices.size() - 1; ++i) {
        if (prices[i] <= 0.0 || prices[i + 1] <= 0.0) continue;
        const double r = std::log(prices[i + 1] / prices[i]);
        const double d = r - mean_ret;
        var += d * d;
    }
    var /= static_cast<double>(count - 1);  // Bessel correction

    result.valid = true;
    result.value = safe_sqrt(var);
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Momentum: (price_now − price_n_ago) / price_n_ago  [fraction]
//
// Simple rate-of-change as a decimal fraction.
// Distinct from rate_of_change() which returns percentage (×100).
// ─────────────────────────────────────────────────────────────────────────────

IndicatorResult IndicatorEngine::momentum(const std::vector<double>& prices, int period) const {
    IndicatorResult result;
    result.name = "MOMENTUM";
    const auto n = static_cast<int>(prices.size());
    const int min_bars = period + 1;

    if (period <= 0) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Momentum: invalid period",
            {{"period", std::to_string(period)}});
        return result;
    }
    if (n < min_bars) {
        result.status = IndicatorStatus::InsufficientData;
        result.sample_count = n;
        result.warmup_remaining = min_bars - n;
        return result;
    }
    if (!validate_price_series(prices)) {
        result.status = IndicatorStatus::InvalidInput;
        if (logger_) logger_->warn("IndicatorEngine", "Momentum: invalid price data detected");
        return result;
    }

    const double current = prices.back();
    const double past = prices[prices.size() - 1 - static_cast<std::size_t>(period)];

    result.valid = true;
    result.value = safe_div(current - past, past);
    result.status = IndicatorStatus::Ok;
    result.sample_count = n;
    result.warmup_remaining = 0;
    return result;
}

} // namespace tb::indicators
