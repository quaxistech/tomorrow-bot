#pragma once
#include "indicator_types.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <vector>
#include <string>

namespace tb::indicators {

// Базовый интерфейс индикатора
class IIndicator {
public:
    virtual ~IIndicator() = default;
    virtual std::string name() const = 0;
    virtual int min_periods() const = 0;
};

// Движок индикаторов — вычисляет технические индикаторы
// Использует TA-Lib если доступна, иначе встроенную реализацию
class IndicatorEngine {
public:
    explicit IndicatorEngine(std::shared_ptr<tb::logging::ILogger> logger);

    bool run_talib_smoke_test();
    bool is_talib_available() const;

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

private:
#ifdef TB_TALIB_AVAILABLE
    IndicatorResult sma_talib(const std::vector<double>& prices, int period) const;
    IndicatorResult ema_talib(const std::vector<double>& prices, int period) const;
#endif
    // Вспомогательный метод: вычисляет полный ряд EMA
    // output[i] — EMA на позиции i; валиден при i >= period-1
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

    bool talib_available_{false};
    std::shared_ptr<tb::logging::ILogger> logger_;
};

} // namespace tb::indicators
