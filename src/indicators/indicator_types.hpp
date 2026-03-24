#pragma once
#include <string>

namespace tb::indicators {

// Результат вычисления индикатора
struct IndicatorResult {
    bool valid{false};
    double value{0.0};
    std::string name;
};

// Результаты MACD
struct MacdResult {
    bool valid{false};
    double macd{0.0};
    double signal{0.0};
    double histogram{0.0};
};

// Результаты Bollinger Bands
struct BollingerResult {
    bool valid{false};
    double upper{0.0};
    double middle{0.0};
    double lower{0.0};
    double bandwidth{0.0};
    double percent_b{0.0};
};

// Результаты ADX
struct AdxResult {
    bool valid{false};
    double adx{0.0};
    double plus_di{0.0};
    double minus_di{0.0};
};

} // namespace tb::indicators
