#pragma once

/**
 * @file pair_scorer.hpp
 * @brief Алгоритм скоринга торговых пар v3 — ТОЛЬКО растущие монеты.
 *
 * Для спотовой торговли прибыль = только от роста цены.
 * Поэтому главный критерий — MOMENTUM (рост за 24ч).
 *
 * ЖЁСТКИЙ ФИЛЬТР: монеты с 24h change < -1% отбрасываются.
 *
 * Веса:
 * 1. Momentum Score (0-40) — ROC 24h + краткосрочный импульс (ГЛАВНЫЙ)
 * 2. Trend Score (0-25) — бычье направление + сила тренда
 * 3. Tradability Score (0-25) — ликвидность + спред + волатильность
 * 4. Quality Score (0-10) — чистота ценового движения
 *
 * ИТОГО: 0-100 баллов. Растущие монеты >> стабильные >> падающие.
 */

#include "pair_scanner_types.hpp"
#include <vector>

namespace tb::pair_scanner {

class PairScorer {
public:
    /// Оценить одну пару. Возвращает score с total_score = -1 если отфильтрована.
    PairScore score(const TickerData& ticker,
                    const std::vector<CandleData>& candles) const;

    /// Momentum score (0-40): ROC 24h + краткосрочное ускорение + EMA slope
    double compute_momentum_score(const TickerData& ticker,
                                   const std::vector<CandleData>& candles) const;

    /// Trend score (0-25): сила + БЫЧЬЕ направление
    double compute_trend_score(const std::vector<CandleData>& candles) const;

    /// Tradability score (0-25): volume порог + spread + volatility
    double compute_tradability_score(const TickerData& ticker,
                                      const std::vector<CandleData>& candles) const;

    /// Quality score (0-10): чистота движения
    double compute_quality_score(const std::vector<CandleData>& candles) const;

private:
    double compute_daily_volatility(const std::vector<CandleData>& candles) const;
    double compute_body_ratio(const std::vector<CandleData>& candles) const;
    double compute_simple_adx(const std::vector<CandleData>& candles) const;
    double compute_bullish_ratio(const std::vector<CandleData>& candles, size_t n) const;
    double compute_roc(const std::vector<CandleData>& candles, size_t n) const;
    double compute_ema_slope(const std::vector<CandleData>& candles, size_t period) const;
};

} // namespace tb::pair_scanner
