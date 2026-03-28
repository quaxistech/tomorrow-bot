#pragma once

/**
 * @file data_validator.hpp
 * @brief Валидатор качества рыночных данных для PairScanner.
 *
 * Проверяет полноту, хронологичность, консистентность свечей и тикеров.
 * Выявляет аномалии: нулевой bid/ask, инвертированный спред, дублирующие
 * таймстампы, пропущенные бары и outlier-значения.
 */

#include "pair_scanner_types.hpp"

#include <vector>
#include <string>
#include <cmath>
#include <set>

namespace tb::pair_scanner {

/// Валидатор качества входных данных для системы отбора пар
class DataValidator {
public:
    /// Проверить качество данных тикера
    static DataQualityFlags validate_ticker(const TickerData& ticker) {
        DataQualityFlags flags;
        flags.has_valid_bid_ask = (ticker.best_bid > 0.0
                                   && ticker.best_ask > 0.0
                                   && ticker.best_ask >= ticker.best_bid);
        flags.has_sufficient_candles = true;
        flags.candles_chronological = true;
        flags.no_duplicate_timestamps = true;
        flags.completeness_ratio = flags.has_valid_bid_ask ? 1.0 : 0.5;
        return flags;
    }

    /// Проверить качество набора свечей
    static DataQualityFlags validate_candles(const std::vector<CandleData>& candles,
                                             int expected_count) {
        DataQualityFlags flags;
        flags.total_candle_count = static_cast<int>(candles.size());
        flags.has_sufficient_candles = (candles.size() >= 15);

        if (candles.empty()) {
            flags.completeness_ratio = 0.0;
            return flags;
        }

        // Проверка хронологического порядка
        flags.candles_chronological = true;
        for (size_t i = 1; i < candles.size(); ++i) {
            if (candles[i].timestamp_ms < candles[i - 1].timestamp_ms) {
                flags.candles_chronological = false;
                break;
            }
        }

        // Проверка дубликатов таймстампов
        std::set<int64_t> seen;
        flags.no_duplicate_timestamps = true;
        for (const auto& c : candles) {
            if (!seen.insert(c.timestamp_ms).second) {
                flags.no_duplicate_timestamps = false;
                break;
            }
        }

        // Подсчёт пропущенных свечей (ожидаем интервал 3600000 мс = 1ч)
        if (candles.size() >= 2) {
            constexpr int64_t kHourMs = 3600000;
            int64_t first_ts = candles.front().timestamp_ms;
            int64_t last_ts = candles.back().timestamp_ms;
            int expected_bars = static_cast<int>((last_ts - first_ts) / kHourMs) + 1;
            flags.missing_candle_count = std::max(0,
                expected_bars - static_cast<int>(candles.size()));
        }

        // Полнота данных
        flags.completeness_ratio = (expected_count > 0)
            ? std::min(1.0, static_cast<double>(candles.size()) / expected_count)
            : 0.0;

        return flags;
    }

    /// Комбинированная валидация тикера + свечей
    static DataQualityFlags validate(const TickerData& ticker,
                                     const std::vector<CandleData>& candles,
                                     int expected_candles) {
        auto ticker_flags = validate_ticker(ticker);
        auto candle_flags = validate_candles(candles, expected_candles);

        DataQualityFlags combined;
        combined.has_valid_bid_ask = ticker_flags.has_valid_bid_ask;
        combined.has_sufficient_candles = candle_flags.has_sufficient_candles;
        combined.candles_chronological = candle_flags.candles_chronological;
        combined.no_duplicate_timestamps = candle_flags.no_duplicate_timestamps;
        combined.missing_candle_count = candle_flags.missing_candle_count;
        combined.total_candle_count = candle_flags.total_candle_count;

        // Общая полнота — минимум из двух измерений
        combined.completeness_ratio = std::min(ticker_flags.completeness_ratio,
                                               candle_flags.completeness_ratio);
        return combined;
    }

    /// Проверить конкретную свечу на аномальные значения
    static bool is_candle_valid(const CandleData& c) {
        if (c.open <= 0.0 || c.high <= 0.0 || c.low <= 0.0 || c.close <= 0.0) return false;
        if (c.high < c.low) return false;
        if (c.open > c.high || c.open < c.low) return false;
        if (c.close > c.high || c.close < c.low) return false;
        if (c.volume < 0.0) return false;
        return true;
    }

    /// Проверить спред на аномалии (инвертированный, слишком широкий)
    static bool is_spread_healthy(const TickerData& ticker, double max_spread_bps) {
        if (ticker.best_ask < ticker.best_bid) return false;
        if (ticker.spread_bps > max_spread_bps && max_spread_bps > 0.0) return false;
        return true;
    }
};

} // namespace tb::pair_scanner
