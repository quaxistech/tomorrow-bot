#include "pair_scorer.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace tb::pair_scanner {

// ========================================================================
// Pair Scorer v3 — ТОЛЬКО РАСТУЩИЕ МОНЕТЫ для спотовой торговли
// ========================================================================
//
// Философия: на СПОТ мы можем зарабатывать ТОЛЬКО на росте цены.
// Поэтому momentum — это 40% всего скора. Падающие монеты
// отсеиваются жёстким фильтром.
//
// Веса: Momentum(40) + Trend(25) + Tradability(25) + Quality(10) = 100
// ========================================================================

PairScore PairScorer::score(const TickerData& ticker,
                            const std::vector<CandleData>& candles) const {
    PairScore result;
    result.symbol = ticker.symbol;
    result.quote_volume_24h = ticker.quote_volume_24h;
    result.change_24h_pct = ticker.change_24h_pct;
    result.avg_spread_bps = ticker.spread_bps;
    result.daily_volatility = compute_daily_volatility(candles);

    // ═══════════════════════════════════════════════════════════════
    // ЖЁСТКИЙ ФИЛЬТР: монеты с падением > 1% за 24ч — отбрасываем.
    // На споте падающая монета = гарантированный убыток.
    // ═══════════════════════════════════════════════════════════════
    if (ticker.change_24h_pct < -1.0) {
        result.total_score = -1.0;
        result.filtered_out = true;
        return result;
    }

    // 1. Momentum Score (0-40) — ДОМИНАНТНЫЙ ФАКТОР
    result.momentum_score = compute_momentum_score(ticker, candles);

    // 2. Trend Score (0-25) — бычье направление + сила
    result.trend_score = compute_trend_score(candles);

    // 3. Tradability Score (0-25) — ликвидность + спред + волатильность
    result.tradability_score = compute_tradability_score(ticker, candles);

    // 4. Quality Score (0-10) — чистота движения
    result.quality_score = compute_quality_score(candles);

    // Суммарный балл
    double raw_total = result.momentum_score
                     + result.trend_score
                     + result.tradability_score
                     + result.quality_score;

    // ═══════════════════════════════════════════════════════════════
    // ШТРАФНОЙ МНОЖИТЕЛЬ: если 24h change отрицательный (но > -1%),
    // значит монета стагнирует — снижаем общий скор на 50%.
    // ═══════════════════════════════════════════════════════════════
    if (ticker.change_24h_pct < 0.0) {
        raw_total *= 0.5;
    }

    result.total_score = std::clamp(raw_total, 0.0, 100.0);
    return result;
}

// ========== Momentum Score (0-40) ==========
// ГЛАВНЫЙ критерий: ROC 24h (20) + краткосрочный ROC 4h (10) + EMA slope (10).
// Для спота: чем выше рост — тем лучше.

double PairScorer::compute_momentum_score(const TickerData& ticker,
                                           const std::vector<CandleData>& candles) const {
    double score = 0.0;

    // --- Компонент 1: ROC за 24ч из тикера (0-20 баллов) ---
    // Это ГЛАВНАЯ метрика. +2% → 8, +5% → 15, +10%+ → 20
    double roc_24h = ticker.change_24h_pct;
    if (roc_24h > 0.0) {
        // Логарифмическая шкала: быстро растёт до 5%, потом замедляется
        // +1% → 6, +2% → 9, +5% → 15, +10% → 18, +15%+ → 20
        score += std::clamp(std::log1p(roc_24h) * 7.4, 0.0, 20.0);
    } else {
        // Отрицательный ROC = штраф (но монета прошла фильтр, значит > -1%)
        // -0.5% → -2, -1% → -4
        score += std::max(-8.0, roc_24h * 4.0);
    }

    // --- Компонент 2: Краткосрочный ROC за 4 свечи (0-10 баллов) ---
    // Ускорение роста за последние 4 часа (подтверждение momentum)
    double roc_4h = compute_roc(candles, 4);
    if (roc_4h > 0.0) {
        // +0.5% за 4ч → 3.5, +1% → 5.8, +2%+ → 10
        score += std::clamp(std::log1p(roc_4h) * 8.3, 0.0, 10.0);
    } else {
        // Замедление роста — небольшой штраф
        score += std::max(-4.0, roc_4h * 2.0);
    }

    // --- Компонент 3: EMA slope за 12 свечей (0-10 баллов) ---
    // Подтверждение устойчивого тренда (EMA растёт = тренд устойчив)
    double slope = compute_ema_slope(candles, 12);
    if (slope > 0.0) {
        // slope 0.1% → 4, 0.2% → 7, 0.3%+ → 10
        score += std::clamp(slope * 35.0, 0.0, 10.0);
    } else {
        // Падающий EMA — штраф
        score += std::max(-5.0, slope * 20.0);
    }

    return std::clamp(score, 0.0, 40.0);
}

// ========== Trend Score (0-25) ==========
// Бычий тренд обязателен для спотовой прибыли.
// Компоненты: ADX сила (0-10) + бычье направление (0-15).
// КЛЮЧЕВОЕ: медвежий тренд = почти 0 баллов.

double PairScorer::compute_trend_score(const std::vector<CandleData>& candles) const {
    if (candles.size() < 15) return 5.0;

    // --- Компонент 1: Сила тренда ADX (0-10 баллов) ---
    double adx = compute_simple_adx(candles);
    double strength_score = 0.0;
    if (adx < 15.0) {
        strength_score = (adx / 15.0) * 3.0;        // Слабый: 0-3
    } else if (adx < 25.0) {
        strength_score = 3.0 + ((adx - 15.0) / 10.0) * 3.0;  // Умеренный: 3-6
    } else if (adx < 50.0) {
        strength_score = 6.0 + ((adx - 25.0) / 25.0) * 4.0;  // Сильный: 6-10
    } else {
        strength_score = 9.5;  // Экстремально сильный
    }

    // --- Компонент 2: Бычье направление (0-15 баллов) ---
    // Анализируем последние 24 свечи: % бычьих + ROC
    double bullish_ratio = compute_bullish_ratio(candles, 24);
    double roc_24 = compute_roc(candles, 24);

    double direction_score = 0.0;

    if (roc_24 > 0.0 && bullish_ratio > 0.50) {
        // БЫЧИЙ ТРЕНД — основной балл
        // ROC contributes 0-8, bullish_ratio contributes 0-7
        double roc_factor = std::clamp(roc_24 / 5.0, 0.0, 1.0);     // 1%→0.2, 5%+→1.0
        double bull_factor = std::clamp((bullish_ratio - 0.50) / 0.25, 0.0, 1.0); // 0.50→0, 0.75+→1.0
        direction_score = roc_factor * 8.0 + bull_factor * 7.0;
    }
    else if (roc_24 > 0.0) {
        // ROC положительный, но мало бычьих свечей — неуверенный тренд
        direction_score = std::clamp(roc_24 / 5.0, 0.0, 1.0) * 5.0;
    }
    else if (bullish_ratio > 0.55) {
        // Больше бычьих свечей, но ROC отрицательный — развернутся?
        direction_score = 3.0;
    }
    else {
        // Медвежий тренд — 0 баллов за направление
        direction_score = 0.0;
    }

    return std::clamp(strength_score + direction_score, 0.0, 25.0);
}

// ========== Tradability Score (0-25) ==========
// Ликвидность + спред + волатильность в одном.
// Минимальные пороги вместо bell curve — не штрафуем растущие монеты.

double PairScorer::compute_tradability_score(const TickerData& ticker,
                                              const std::vector<CandleData>& candles) const {
    double score = 0.0;

    // --- Компонент 1: Volume (0-8 баллов) ---
    // Пороговая модель: $500K достаточно для $10-30 ордеров.
    // Выше $1M → отлично, выше $5M → максимум.
    double vol = ticker.quote_volume_24h;
    if (vol >= 5'000'000.0) {
        score += 8.0;
    } else if (vol >= 1'000'000.0) {
        score += 5.0 + 3.0 * (vol - 1'000'000.0) / 4'000'000.0;
    } else if (vol >= 500'000.0) {
        score += 3.0 + 2.0 * (vol - 500'000.0) / 500'000.0;
    } else if (vol >= 100'000.0) {
        score += 1.0 + 2.0 * (vol - 100'000.0) / 400'000.0;
    } else {
        score += vol / 100'000.0;
    }

    // --- Компонент 2: Spread (0-10 баллов) ---
    // Экспоненциальное затухание: чем уже спред, тем лучше.
    // 1 bps → 9.4, 5 bps → 7.2, 10 bps → 5.1, 20 bps → 2.6, 50 bps → 0.4
    // spread=0 означает отсутствие данных (bid/ask=0) — даём 0 баллов
    double spread = ticker.spread_bps;
    if (spread > 0.0) {
        score += std::clamp(10.0 * std::exp(-spread / 15.0), 0.0, 10.0);
    }
    // spread == 0.0 → нет данных → 0 баллов (а не максимум)

    // --- Компонент 3: Volatility (0-7 баллов) ---
    // Для спотовой краткосрочки: волатильность = возможность.
    // НЕ штрафуем высокую волатильность (это growth potential).
    // Штрафуем только ОЧЕНЬ низкую (< 0.5% — нет движения) и
    // ЭКСТРЕМАЛЬНО высокую (> 20% — манипулятивные shitcoin).
    double daily_vol = compute_daily_volatility(candles);
    if (daily_vol < 0.5) {
        score += daily_vol / 0.5 * 3.0;   // 0-3 для < 0.5%
    } else if (daily_vol < 20.0) {
        score += 5.0 + std::min(2.0, daily_vol / 10.0);  // 5-7 для 0.5-20%
    } else {
        score += std::max(0.0, 4.0 - (daily_vol - 20.0) / 10.0);  // Штраф > 20%
    }

    return std::clamp(score, 0.0, 25.0);
}

// ========== Quality Score (0-10) ==========
// Чистота ценового движения. Высокий body ratio = уверенное направленное движение.

double PairScorer::compute_quality_score(const std::vector<CandleData>& candles) const {
    double body_ratio = compute_body_ratio(candles);
    // body_ratio 0.7 → 7, 1.0 → 10
    return std::clamp(body_ratio * 10.0, 0.0, 10.0);
}

// ========== Вспомогательные методы ==========

double PairScorer::compute_daily_volatility(const std::vector<CandleData>& candles) const {
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
    return stddev * std::sqrt(24.0) * 100.0;
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

double PairScorer::compute_bullish_ratio(const std::vector<CandleData>& candles, size_t n) const {
    if (candles.empty()) return 0.5;

    size_t count = std::min(n, candles.size());
    size_t start = candles.size() - count;
    size_t bullish = 0;

    for (size_t i = start; i < candles.size(); ++i) {
        if (candles[i].close > candles[i].open) {
            ++bullish;
        }
    }

    return static_cast<double>(bullish) / count;
}

double PairScorer::compute_roc(const std::vector<CandleData>& candles, size_t n) const {
    if (candles.size() < n + 1 || n == 0) return 0.0;

    size_t end_idx = candles.size() - 1;
    size_t start_idx = candles.size() - 1 - n;

    double start_price = candles[start_idx].close;
    double end_price = candles[end_idx].close;

    if (start_price <= 0.0) return 0.0;

    return ((end_price - start_price) / start_price) * 100.0;
}

double PairScorer::compute_ema_slope(const std::vector<CandleData>& candles, size_t period) const {
    if (candles.size() < period + 2) return 0.0;

    double multiplier = 2.0 / (period + 1);
    double ema = candles[0].close;
    double prev_ema = 0.0;

    for (size_t i = 1; i < candles.size(); ++i) {
        prev_ema = ema;
        ema = candles[i].close * multiplier + ema * (1.0 - multiplier);
    }

    if (prev_ema <= 0.0) return 0.0;
    return ((ema - prev_ema) / prev_ema) * 100.0;
}

double PairScorer::compute_simple_adx(const std::vector<CandleData>& candles) const {
    const size_t period = 14;
    if (candles.size() < period + 2) return 10.0;

    std::vector<double> plus_dm, minus_dm, tr;
    for (size_t i = 1; i < candles.size(); ++i) {
        double high_diff = candles[i].high - candles[i - 1].high;
        double low_diff = candles[i - 1].low - candles[i].low;

        double pdm = (high_diff > low_diff && high_diff > 0.0) ? high_diff : 0.0;
        double ndm = (low_diff > high_diff && low_diff > 0.0) ? low_diff : 0.0;

        double h_l = candles[i].high - candles[i].low;
        double h_cp = std::abs(candles[i].high - candles[i - 1].close);
        double l_cp = std::abs(candles[i].low - candles[i - 1].close);
        double true_range = std::max({h_l, h_cp, l_cp});

        plus_dm.push_back(pdm);
        minus_dm.push_back(ndm);
        tr.push_back(true_range);
    }

    if (tr.size() < period) return 10.0;

    // Сглаживание Вайлдера
    auto wilder_smooth = [&](const std::vector<double>& data) -> std::vector<double> {
        std::vector<double> smoothed;
        double sum = 0.0;
        for (size_t i = 0; i < period; ++i) sum += data[i];
        smoothed.push_back(sum);
        for (size_t i = period; i < data.size(); ++i) {
            double val = smoothed.back() - smoothed.back() / period + data[i];
            smoothed.push_back(val);
        }
        return smoothed;
    };

    auto sm_pdm = wilder_smooth(plus_dm);
    auto sm_ndm = wilder_smooth(minus_dm);
    auto sm_tr = wilder_smooth(tr);

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

    double adx = 0.0;
    for (size_t i = 0; i < period; ++i) adx += dx_vals[i];
    adx /= period;
    for (size_t i = period; i < dx_vals.size(); ++i) {
        adx = (adx * (period - 1) + dx_vals[i]) / period;
    }

    return adx;
}

} // namespace tb::pair_scanner
