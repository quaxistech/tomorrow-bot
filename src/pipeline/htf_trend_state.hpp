#pragma once
/**
 * @file htf_trend_state.hpp
 * @brief Агрегатор HTF (1-hour timeframe) индикаторов и буферов (D6: extract из TradingPipeline).
 *
 * Собирает 16 данных-полей старшего таймфрейма в одну структуру:
 *   - Индикаторы: EMA 20/50, RSI 14, ADX 14, MACD histogram, last_close.
 *   - Direction/strength тренда + valid-флаг.
 *   - Кольцевые буферы closes/highs/lows (макс 200 свечей).
 *   - Timestamps и флаги интервалов обновления.
 *
 * Логика расчёта/обновления остаётся в `TradingPipeline` (использует REST + IndicatorEngine).
 * Этот класс — pure data + дисциплина доступа: методы validate(), reset(), append_candle().
 *
 * Thread-safety: класс предполагает доступ под `pipeline_mutex_`. Не имеет встроенных мьютексов.
 */

#include <cstdint>
#include <vector>

namespace tb::pipeline {

class HtfTrendState {
public:
    static constexpr std::size_t kBufferMaxSize = 200;
    static constexpr std::int64_t kRefreshIntervalNs = 3600'000'000'000LL; // 60 минут

    // ---- Индикаторы старшего таймфрейма ----
    double ema_20{0.0};
    double ema_50{0.0};
    double rsi_14{50.0};
    double adx{0.0};
    double macd_histogram{0.0};
    double last_close{0.0};

    /// HTF данные валидны (достаточно истории и расчёт прошёл).
    bool valid{false};

    // ---- Trend ----
    /// Направление: +1=up, -1=down, 0=range.
    int trend_direction{0};
    /// Сила тренда [0..1].
    double trend_strength{0.0};

    // ---- Буферы ----
    std::vector<double> closes_buffer;
    std::vector<double> highs_buffer;
    std::vector<double> lows_buffer;

    // ---- Тайминг обновления ----
    std::int64_t last_update_ns{0};
    /// Флаг экстренного обновления (price moved > 3×ATR vs last_close).
    bool urgent_update_needed{false};

    /// Должно ли произойти периодическое обновление (по интервалу) или urgent.
    [[nodiscard]] bool should_refresh(std::int64_t now_ns) const noexcept {
        if (urgent_update_needed) return true;
        return last_update_ns == 0 || (now_ns - last_update_ns) >= kRefreshIntervalNs;
    }

    /// Сбросить состояние (для recovery).
    void reset() noexcept {
        ema_20 = 0.0; ema_50 = 0.0; rsi_14 = 50.0; adx = 0.0;
        macd_histogram = 0.0; last_close = 0.0;
        valid = false;
        trend_direction = 0;
        trend_strength = 0.0;
        closes_buffer.clear();
        highs_buffer.clear();
        lows_buffer.clear();
        last_update_ns = 0;
        urgent_update_needed = false;
    }

    /// Добавить candle в буфер с обрезкой до max-size.
    void append_candle(double close_, double high_, double low_) {
        closes_buffer.push_back(close_);
        highs_buffer.push_back(high_);
        lows_buffer.push_back(low_);
        while (closes_buffer.size() > kBufferMaxSize) closes_buffer.erase(closes_buffer.begin());
        while (highs_buffer.size()  > kBufferMaxSize) highs_buffer.erase(highs_buffer.begin());
        while (lows_buffer.size()   > kBufferMaxSize) lows_buffer.erase(lows_buffer.begin());
        last_close = close_;
    }
};

} // namespace tb::pipeline
