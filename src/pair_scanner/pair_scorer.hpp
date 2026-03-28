#pragma once

/**
 * @file pair_scorer.hpp
 * @brief Алгоритм скоринга торговых пар v4 — acceleration-based selection.
 *
 * Для спотовой торговли прибыль = только от роста цены.
 * Вместо отбора монет по magnitude (уже pumped), ищем УСКОРЕНИЕ:
 * монеты, где 4h momentum выше среднего 24h темпа = рост усиливается.
 *
 * ЖЁСТКИЕ ФИЛЬТРЫ:
 * - 24h change < -1% → отбрасывается
 * - 24h change > 30% но 4h ROC < 15% от 24h → exhausted pump, отбрасывается
 *
 * Веса:
 * 1. Momentum Score (0-40) — recent 4h ROC (20) + acceleration (15) + fresh start (5)
 * 2. Trend Score (0-25) — бычье направление + сила тренда
 * 3. Tradability Score (0-25) — ликвидность + спред + волатильность
 * 4. Quality Score (0-10) — чистота ценового движения
 *
 * ИТОГО: 0-100 баллов.
 */

#include "pair_scanner_types.hpp"
#include "config/config_types.hpp"
#include <vector>

namespace tb::pair_scanner {

class PairScorer {
public:
    /// Конструктор с конфигурацией scorer-а
    explicit PairScorer(config::ScorerConfig config = {});

    /// Оценить одну пару. Возвращает score с total_score = -1 если отфильтрована.
    PairScore score(const TickerData& ticker,
                    const std::vector<CandleData>& candles) const;

    /// Momentum score (0-40): recent 4h ROC + acceleration + fresh start bonus
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
    config::ScorerConfig config_;

    double compute_daily_volatility(const std::vector<CandleData>& candles) const;
    double compute_body_ratio(const std::vector<CandleData>& candles) const;
    double compute_simple_adx(const std::vector<CandleData>& candles) const;
    double compute_bullish_ratio(const std::vector<CandleData>& candles, size_t n) const;
    double compute_roc(const std::vector<CandleData>& candles, size_t n) const;
    double compute_ema_slope(const std::vector<CandleData>& candles, size_t period) const;
};

} // namespace tb::pair_scanner
