#include "indicator_engine.hpp"
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <algorithm>

namespace tb::indicators {

IndicatorEngine::IndicatorEngine(std::shared_ptr<tb::logging::ILogger> logger)
    : logger_(std::move(logger))
    , talib_available_(false) // TA-Lib не интегрирована, используется встроенная реализация
{
    if (logger_) {
        logger_->info("IndicatorEngine", "Используется встроенная реализация индикаторов");
    }
}

bool IndicatorEngine::is_talib_available() const {
    return talib_available_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Вспомогательный метод: полный ряд EMA
// output[i] — EMA на позиции i; для i < period-1 значение равно 0.0 (невалидно)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<double> IndicatorEngine::ema_series(const std::vector<double>& prices, int period) const {
    const std::size_t n = prices.size();
    std::vector<double> out(n, 0.0);
    if (n < static_cast<std::size_t>(period) || period <= 0) return out;

    // Первое значение EMA = SMA первых period элементов
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
#ifdef TB_TALIB_AVAILABLE
    return sma_talib(prices, period);
#else
    return sma_builtin(prices, period);
#endif
}

IndicatorResult IndicatorEngine::sma_builtin(const std::vector<double>& prices, int period) const {
    if (period <= 0 || prices.size() < static_cast<std::size_t>(period)) {
        return {false, 0.0, "SMA"};
    }
    const std::size_t n = prices.size();
    double sum = 0.0;
    for (std::size_t i = n - static_cast<std::size_t>(period); i < n; ++i) {
        sum += prices[i];
    }
    return {true, sum / static_cast<double>(period), "SMA"};
}

// ─────────────────────────────────────────────────────────────────────────────
// EMA
// ─────────────────────────────────────────────────────────────────────────────
IndicatorResult IndicatorEngine::ema(const std::vector<double>& prices, int period) const {
#ifdef TB_TALIB_AVAILABLE
    return ema_talib(prices, period);
#else
    return ema_builtin(prices, period);
#endif
}

IndicatorResult IndicatorEngine::ema_builtin(const std::vector<double>& prices, int period) const {
    if (period <= 0 || prices.size() < static_cast<std::size_t>(period)) {
        return {false, 0.0, "EMA"};
    }
    const auto series = ema_series(prices, period);
    return {true, series.back(), "EMA"};
}

// ─────────────────────────────────────────────────────────────────────────────
// RSI (метод Уайлдера)
// ─────────────────────────────────────────────────────────────────────────────
IndicatorResult IndicatorEngine::rsi(const std::vector<double>& prices, int period) const {
    return rsi_builtin(prices, period);
}

IndicatorResult IndicatorEngine::rsi_builtin(const std::vector<double>& prices, int period) const {
    const std::size_t n = prices.size();
    if (period <= 0 || n < static_cast<std::size_t>(period) + 1) {
        return {false, 0.0, "RSI"};
    }

    // Вычисляем первые изменения цены
    double avg_gain = 0.0, avg_loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        const double delta = prices[i] - prices[i - 1];
        if (delta > 0.0) avg_gain += delta;
        else             avg_loss += -delta;
    }
    avg_gain /= static_cast<double>(period);
    avg_loss /= static_cast<double>(period);

    // Сглаживание по Уайлдеру для остальных баров
    for (std::size_t i = static_cast<std::size_t>(period) + 1; i < n; ++i) {
        const double delta = prices[i] - prices[i - 1];
        const double gain = delta > 0.0 ? delta : 0.0;
        const double loss = delta < 0.0 ? -delta : 0.0;
        avg_gain = (avg_gain * static_cast<double>(period - 1) + gain) / static_cast<double>(period);
        avg_loss = (avg_loss * static_cast<double>(period - 1) + loss) / static_cast<double>(period);
    }

    if (avg_loss < 1e-12) return {true, 100.0, "RSI"};
    const double rs = avg_gain / avg_loss;
    return {true, 100.0 - 100.0 / (1.0 + rs), "RSI"};
}

// ─────────────────────────────────────────────────────────────────────────────
// MACD
// ─────────────────────────────────────────────────────────────────────────────
MacdResult IndicatorEngine::macd(const std::vector<double>& prices,
                                  int fast_period, int slow_period, int signal_period) const {
    return macd_builtin(prices, fast_period, slow_period, signal_period);
}

MacdResult IndicatorEngine::macd_builtin(const std::vector<double>& prices,
                                          int fast, int slow, int signal) const {
    const std::size_t n = prices.size();
    // Нужно как минимум slow баров для первого EMA + signal баров для сигнальной линии
    if (fast <= 0 || slow <= 0 || signal <= 0 || fast >= slow ||
        n < static_cast<std::size_t>(slow + signal - 1)) {
        return {};
    }

    // Полные ряды EMA
    const auto fast_ema  = ema_series(prices, fast);
    const auto slow_ema  = ema_series(prices, slow);

    // Ряд MACD: валиден с индекса slow-1
    const std::size_t macd_start = static_cast<std::size_t>(slow - 1);
    const std::size_t macd_len   = n - macd_start;
    std::vector<double> macd_line(macd_len);
    for (std::size_t i = 0; i < macd_len; ++i) {
        macd_line[i] = fast_ema[macd_start + i] - slow_ema[macd_start + i];
    }

    if (macd_line.size() < static_cast<std::size_t>(signal)) return {};

    // Сигнальная линия = EMA(macd_line, signal)
    const auto signal_series = ema_series(macd_line, signal);

    const double macd_val  = macd_line.back();
    const double sig_val   = signal_series.back();
    return {true, macd_val, sig_val, macd_val - sig_val};
}

// ─────────────────────────────────────────────────────────────────────────────
// Bollinger Bands
// ─────────────────────────────────────────────────────────────────────────────
BollingerResult IndicatorEngine::bollinger(const std::vector<double>& prices,
                                            int period, double stddev_mult) const {
    return bollinger_builtin(prices, period, stddev_mult);
}

BollingerResult IndicatorEngine::bollinger_builtin(const std::vector<double>& prices,
                                                    int period, double stddev_mult) const {
    const std::size_t n = prices.size();
    if (period <= 0 || n < static_cast<std::size_t>(period)) return {};

    const std::size_t start = n - static_cast<std::size_t>(period);

    // Среднее (SMA)
    double sum = 0.0;
    for (std::size_t i = start; i < n; ++i) sum += prices[i];
    const double middle = sum / static_cast<double>(period);

    // Стандартное отклонение (несмещённое, делитель = period)
    double var = 0.0;
    for (std::size_t i = start; i < n; ++i) {
        const double d = prices[i] - middle;
        var += d * d;
    }
    const double sigma = std::sqrt(var / static_cast<double>(period));

    const double upper = middle + stddev_mult * sigma;
    const double lower = middle - stddev_mult * sigma;
    const double bandwidth = (upper - lower) / (middle != 0.0 ? middle : 1.0);

    double percent_b = 0.5;
    if (upper - lower > 1e-12) {
        percent_b = (prices.back() - lower) / (upper - lower);
    }

    return {true, upper, middle, lower, bandwidth, percent_b};
}

// ─────────────────────────────────────────────────────────────────────────────
// ATR (Average True Range, метод Уайлдера)
// ─────────────────────────────────────────────────────────────────────────────
IndicatorResult IndicatorEngine::atr(const std::vector<double>& high,
                                      const std::vector<double>& low,
                                      const std::vector<double>& close,
                                      int period) const {
    return atr_builtin(high, low, close, period);
}

IndicatorResult IndicatorEngine::atr_builtin(const std::vector<double>& high,
                                              const std::vector<double>& low,
                                              const std::vector<double>& close,
                                              int period) const {
    const std::size_t n = high.size();
    if (period <= 0 || n != low.size() || n != close.size() ||
        n < static_cast<std::size_t>(period) + 1) {
        return {false, 0.0, "ATR"};
    }

    // True Range для каждого бара (начиная с индекса 1)
    std::vector<double> tr(n - 1);
    for (std::size_t i = 1; i < n; ++i) {
        const double hl   = high[i] - low[i];
        const double hpc  = std::abs(high[i]  - close[i - 1]);
        const double lpc  = std::abs(low[i]   - close[i - 1]);
        tr[i - 1] = std::max({hl, hpc, lpc});
    }

    // Первый ATR = среднее первых period значений TR
    double atr_val = 0.0;
    for (int i = 0; i < period; ++i) atr_val += tr[i];
    atr_val /= static_cast<double>(period);

    // Сглаживание по Уайлдеру
    const std::size_t tr_n = tr.size();
    for (std::size_t i = static_cast<std::size_t>(period); i < tr_n; ++i) {
        atr_val = (atr_val * static_cast<double>(period - 1) + tr[i]) / static_cast<double>(period);
    }

    return {true, atr_val, "ATR"};
}

// ─────────────────────────────────────────────────────────────────────────────
// ADX (+DI, -DI, ADX)
// ─────────────────────────────────────────────────────────────────────────────
AdxResult IndicatorEngine::adx(const std::vector<double>& high,
                                 const std::vector<double>& low,
                                 const std::vector<double>& close,
                                 int period) const {
    return adx_builtin(high, low, close, period);
}

AdxResult IndicatorEngine::adx_builtin(const std::vector<double>& high,
                                         const std::vector<double>& low,
                                         const std::vector<double>& close,
                                         int period) const {
    const std::size_t n = high.size();
    if (period <= 0 || n != low.size() || n != close.size() ||
        n < static_cast<std::size_t>(2 * period + 1)) {
        return {};
    }

    // Вычисляем +DM, -DM и TR для каждого бара
    const std::size_t bars = n - 1;
    std::vector<double> plus_dm(bars), minus_dm(bars), tr_v(bars);
    for (std::size_t i = 1; i < n; ++i) {
        const double up   = high[i]  - high[i - 1];
        const double down = low[i - 1] - low[i];

        plus_dm[i - 1]  = (up > down && up > 0.0) ? up   : 0.0;
        minus_dm[i - 1] = (down > up && down > 0.0) ? down : 0.0;

        const double hl  = high[i] - low[i];
        const double hpc = std::abs(high[i]  - close[i - 1]);
        const double lpc = std::abs(low[i]   - close[i - 1]);
        tr_v[i - 1] = std::max({hl, hpc, lpc});
    }

    // Первое сглаженное значение (сумма первых period баров)
    double smooth_pdm = 0.0, smooth_mdm = 0.0, smooth_tr = 0.0;
    for (int i = 0; i < period; ++i) {
        smooth_pdm += plus_dm[i];
        smooth_mdm += minus_dm[i];
        smooth_tr  += tr_v[i];
    }

    // Ряд DX-значений для вычисления ADX
    std::vector<double> dx_series;
    dx_series.reserve(bars - static_cast<std::size_t>(period));

    auto calc_dx = [&]() -> double {
        if (smooth_tr < 1e-12) return 0.0;
        const double plus_di  = 100.0 * smooth_pdm / smooth_tr;
        const double minus_di = 100.0 * smooth_mdm / smooth_tr;
        const double di_sum   = plus_di + minus_di;
        return di_sum > 1e-12 ? 100.0 * std::abs(plus_di - minus_di) / di_sum : 0.0;
    };

    dx_series.push_back(calc_dx());

    // Продолжаем сглаживание по Уайлдеру для оставшихся баров
    for (std::size_t i = static_cast<std::size_t>(period); i < bars; ++i) {
        smooth_pdm = smooth_pdm - smooth_pdm / static_cast<double>(period) + plus_dm[i];
        smooth_mdm = smooth_mdm - smooth_mdm / static_cast<double>(period) + minus_dm[i];
        smooth_tr  = smooth_tr  - smooth_tr  / static_cast<double>(period) + tr_v[i];
        dx_series.push_back(calc_dx());
    }

    // Первый ADX = среднее первых period значений DX
    if (dx_series.size() < static_cast<std::size_t>(period)) return {};

    double adx_val = 0.0;
    for (int i = 0; i < period; ++i) adx_val += dx_series[i];
    adx_val /= static_cast<double>(period);

    // Сглаживание ADX
    for (std::size_t i = static_cast<std::size_t>(period); i < dx_series.size(); ++i) {
        adx_val = (adx_val * static_cast<double>(period - 1) + dx_series[i]) / static_cast<double>(period);
    }

    // Финальные значения +DI и -DI
    const double plus_di  = smooth_tr > 1e-12 ? 100.0 * smooth_pdm / smooth_tr : 0.0;
    const double minus_di = smooth_tr > 1e-12 ? 100.0 * smooth_mdm / smooth_tr : 0.0;

    return {true, adx_val, plus_di, minus_di};
}

// ─────────────────────────────────────────────────────────────────────────────
// OBV (On-Balance Volume)
// ─────────────────────────────────────────────────────────────────────────────
IndicatorResult IndicatorEngine::obv(const std::vector<double>& prices,
                                      const std::vector<double>& volumes) const {
    return obv_builtin(prices, volumes);
}

IndicatorResult IndicatorEngine::obv_builtin(const std::vector<double>& prices,
                                              const std::vector<double>& volumes) const {
    const std::size_t n = prices.size();
    if (n < 2 || n != volumes.size()) return {false, 0.0, "OBV"};

    double obv_val = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        if (prices[i] > prices[i - 1])      obv_val += volumes[i];
        else if (prices[i] < prices[i - 1]) obv_val -= volumes[i];
        // При равенстве цен OBV не меняется
    }
    return {true, obv_val, "OBV"};
}

// ─────────────────────────────────────────────────────────────────────────────
// VWAP (скользящий, не сессионный)
// ─────────────────────────────────────────────────────────────────────────────
IndicatorResult IndicatorEngine::vwap(const std::vector<double>& high,
                                       const std::vector<double>& low,
                                       const std::vector<double>& close,
                                       const std::vector<double>& volume) const {
    const std::size_t n = high.size();
    if (n == 0 || n != low.size() || n != close.size() || n != volume.size()) {
        return {false, 0.0, "VWAP"};
    }

    double sum_tpv = 0.0, sum_vol = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double typical = (high[i] + low[i] + close[i]) / 3.0;
        sum_tpv += typical * volume[i];
        sum_vol += volume[i];
    }

    if (sum_vol < 1e-12) return {false, 0.0, "VWAP"};
    return {true, sum_tpv / sum_vol, "VWAP"};
}

} // namespace tb::indicators
