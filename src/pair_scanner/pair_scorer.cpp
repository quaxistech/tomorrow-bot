#include "pair_scorer.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace tb::pair_scanner {

// ========== Публичный API ==========

PairScore PairScorer::score(const TickerData& ticker,
                            const std::vector<CandleData>& candles) const {
    PairScore result;
    result.symbol = ticker.symbol;
    result.quote_volume_24h = ticker.quote_volume_24h;

    // 1. Volume Score — ликвидность
    result.volume_score = compute_volume_score(ticker.quote_volume_24h);

    // 2. Volatility Score — возможность заработка
    result.volatility_score = compute_volatility_score(candles);
    result.daily_volatility = compute_daily_volatility(candles);

    // 3. Spread Score — стоимость входа/выхода
    result.spread_score = compute_spread_score(ticker.spread_bps);
    result.avg_spread_bps = ticker.spread_bps;

    // 4. Trend Score — ясность направления
    result.trend_score = compute_trend_score(candles);

    // 5. Quality Score — чистота движения
    result.quality_score = compute_quality_score(candles);

    // Суммарный балл 0-100
    result.total_score = result.volume_score
                       + result.volatility_score
                       + result.spread_score
                       + result.trend_score
                       + result.quality_score;

    return result;
}

// ========== Volume Score (0-20) ==========
// Логарифмическая шкала: чем больше объём — тем лучше ликвидность.
// $50K → 0, $500K → 8, $5M → 14, $50M → 18, $500M+ → 20

double PairScorer::compute_volume_score(double quote_volume_24h) const {
    if (quote_volume_24h <= 0.0) return 0.0;

    // log10($500K) ≈ 5.7, log10($50M) ≈ 7.7, log10($500M) ≈ 8.7
    double log_vol = std::log10(quote_volume_24h);

    // Линейная интерполяция: log10(50K)=4.7 → 0, log10(500M)=8.7 → 20
    constexpr double kMinLog = 4.7;   // $50K
    constexpr double kMaxLog = 8.7;   // $500M
    double normalized = (log_vol - kMinLog) / (kMaxLog - kMinLog);
    return std::clamp(normalized * 20.0, 0.0, 20.0);
}

// ========== Volatility Score (0-20) ==========
// Оптимальная дневная волатильность для торговли: 2-5%.
// Слишком низкая = нет возможностей, слишком высокая = слишком рискованно.

double PairScorer::compute_volatility_score(const std::vector<CandleData>& candles) const {
    double vol = compute_daily_volatility(candles);
    if (vol <= 0.0) return 0.0;

    // Колоколообразная функция с пиком на 3.5%
    // optimal_center = 3.5%, optimal_width = 3.0%
    constexpr double kCenter = 3.5;
    constexpr double kWidth = 3.0;
    double deviation = std::abs(vol - kCenter) / kWidth;
    double score = std::exp(-deviation * deviation) * 20.0;

    // Штраф за экстремальную волатильность (>15%) или минимальную (<0.3%)
    if (vol > 15.0) score *= 0.2;
    if (vol < 0.3) score *= 0.3;

    return std::clamp(score, 0.0, 20.0);
}

// ========== Spread Score (0-20) ==========
// Спред — прямая стоимость входа/выхода из позиции.
// 0 bps → 20, 5 bps → 18, 15 bps → 12, 30 bps → 5, 50+ bps → 0

double PairScorer::compute_spread_score(double spread_bps) const {
    if (spread_bps < 0.0) return 0.0;

    // Экспоненциальное затухание: score = 20 * exp(-spread/15)
    double score = 20.0 * std::exp(-spread_bps / 15.0);
    return std::clamp(score, 0.0, 20.0);
}

// ========== Trend Score (0-20) ==========
// Сила направленного движения. Если пара трендовая — стратегии работают лучше.
// Используем упрощённый ADX из свечей.

double PairScorer::compute_trend_score(const std::vector<CandleData>& candles) const {
    if (candles.size() < 15) return 5.0;  // Недостаточно данных — средний балл

    double adx = compute_simple_adx(candles);

    // ADX интерпретация:
    // 0-15  → слабый тренд (choppy) → 0-8 баллов
    // 15-25 → умеренный тренд → 8-15 баллов
    // 25-50 → сильный тренд → 15-20 баллов
    // 50+   → экстремальный тренд → 18-20 (немного штраф за переполненность)
    if (adx < 15.0) {
        return (adx / 15.0) * 8.0;
    } else if (adx < 25.0) {
        return 8.0 + ((adx - 15.0) / 10.0) * 7.0;
    } else if (adx < 50.0) {
        return 15.0 + ((adx - 25.0) / 25.0) * 5.0;
    } else {
        return 19.0;  // Экстремально сильный тренд — хорошо, но без переоценки
    }
}

// ========== Quality Score (0-20) ==========
// Отношение тела свечей к полному диапазону. Чистое движение vs шум.
// body_ratio = 1.0 → чистый тренд (marubozu), body_ratio = 0.0 → doji/шум

double PairScorer::compute_quality_score(const std::vector<CandleData>& candles) const {
    double body_ratio = compute_body_ratio(candles);
    // body_ratio 0.0 → 0, 0.3 → 10, 0.5 → 15, 0.7+ → 20
    double score = body_ratio * 28.0;  // 0.7 * 28 ≈ 20
    return std::clamp(score, 0.0, 20.0);
}

// ========== Вспомогательные методы ==========

double PairScorer::compute_daily_volatility(const std::vector<CandleData>& candles) const {
    // Считаем из часовых свечей: stddev(returns) * sqrt(24) ≈ дневная волатильность
    if (candles.size() < 3) return 0.0;

    std::vector<double> returns;
    returns.reserve(candles.size() - 1);
    for (size_t i = 1; i < candles.size(); ++i) {
        if (candles[i - 1].close > 0.0) {
            double ret = (candles[i].close - candles[i - 1].close) / candles[i - 1].close;
            returns.push_back(ret);
        }
    }

    if (returns.size() < 2) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sq_sum = 0.0;
    for (double r : returns) {
        sq_sum += (r - mean) * (r - mean);
    }
    double stddev = std::sqrt(sq_sum / (returns.size() - 1));

    // Пересчёт из часовой в дневную волатильность (× sqrt(24))
    return stddev * std::sqrt(24.0) * 100.0;  // в процентах
}

double PairScorer::compute_body_ratio(const std::vector<CandleData>& candles) const {
    if (candles.empty()) return 0.0;

    double total_body = 0.0;
    double total_range = 0.0;

    for (const auto& c : candles) {
        double range = c.high - c.low;
        double body = std::abs(c.close - c.open);
        if (range > 0.0) {
            total_body += body;
            total_range += range;
        }
    }

    return (total_range > 0.0) ? (total_body / total_range) : 0.0;
}

double PairScorer::compute_simple_adx(const std::vector<CandleData>& candles) const {
    // Упрощённый ADX без TA-Lib:
    // 1. Вычисляем +DM и -DM для каждого бара
    // 2. Сглаживаем (EMA-14)
    // 3. Вычисляем DX = |+DI - -DI| / (+DI + -DI)
    // 4. ADX = EMA(DX, 14)

    const size_t period = 14;
    if (candles.size() < period + 2) return 10.0;  // Мало данных — средний балл

    std::vector<double> plus_dm, minus_dm, tr;
    for (size_t i = 1; i < candles.size(); ++i) {
        double high_diff = candles[i].high - candles[i - 1].high;
        double low_diff = candles[i - 1].low - candles[i].low;

        // +DM: если рост максимума > падения минимума, и > 0
        double pdm = (high_diff > low_diff && high_diff > 0.0) ? high_diff : 0.0;
        // -DM: если падение минимума > роста максимума, и > 0
        double ndm = (low_diff > high_diff && low_diff > 0.0) ? low_diff : 0.0;

        // True Range
        double h_l = candles[i].high - candles[i].low;
        double h_cp = std::abs(candles[i].high - candles[i - 1].close);
        double l_cp = std::abs(candles[i].low - candles[i - 1].close);
        double true_range = std::max({h_l, h_cp, l_cp});

        plus_dm.push_back(pdm);
        minus_dm.push_back(ndm);
        tr.push_back(true_range);
    }

    if (tr.size() < period) return 10.0;

    // Сглаживание Вайлдера (seed = SMA, затем: prev * (period-1)/period + curr / period)
    auto wilder_smooth = [&](const std::vector<double>& data) -> std::vector<double> {
        std::vector<double> smoothed;
        double sum = 0.0;
        for (size_t i = 0; i < period; ++i) sum += data[i];
        smoothed.push_back(sum);  // seed = сумма (не среднее — ADX считает отношения)
        for (size_t i = period; i < data.size(); ++i) {
            double val = smoothed.back() - smoothed.back() / period + data[i];
            smoothed.push_back(val);
        }
        return smoothed;
    };

    auto sm_pdm = wilder_smooth(plus_dm);
    auto sm_ndm = wilder_smooth(minus_dm);
    auto sm_tr = wilder_smooth(tr);

    // Вычисляем DX для каждого бара
    std::vector<double> dx_vals;
    size_t n = std::min({sm_pdm.size(), sm_ndm.size(), sm_tr.size()});
    for (size_t i = 0; i < n; ++i) {
        if (sm_tr[i] <= 0.0) continue;
        double plus_di = sm_pdm[i] / sm_tr[i] * 100.0;
        double minus_di = sm_ndm[i] / sm_tr[i] * 100.0;
        double di_sum = plus_di + minus_di;
        if (di_sum > 0.0) {
            dx_vals.push_back(std::abs(plus_di - minus_di) / di_sum * 100.0);
        }
    }

    if (dx_vals.size() < period) return 10.0;

    // ADX = EMA(DX, period)
    double adx = 0.0;
    for (size_t i = 0; i < period; ++i) adx += dx_vals[i];
    adx /= period;
    for (size_t i = period; i < dx_vals.size(); ++i) {
        adx = (adx * (period - 1) + dx_vals[i]) / period;
    }

    return adx;
}

} // namespace tb::pair_scanner
