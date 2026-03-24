#pragma once

/**
 * @file pair_scorer.hpp
 * @brief Алгоритм скоринга торговых пар.
 *
 * Оценивает каждую пару по 5 независимым критериям (0-20 баллов каждый):
 * 1. Volume Score — ликвидность (24ч объём USDT, log-шкала)
 * 2. Volatility Score — возможность заработка (оптимум 2-5% дневная)
 * 3. Spread Score — стоимость входа/выхода (ниже = лучше)
 * 4. Trend Score — ясность направления (ADX-подобная метрика из свечей)
 * 5. Quality Score — качество ценового движения (body/shadow ratio)
 */

#include "pair_scanner_types.hpp"
#include <vector>

namespace tb::pair_scanner {

class PairScorer {
public:
    /// Оценить одну пару на основе тикера и исторических свечей
    PairScore score(const TickerData& ticker,
                    const std::vector<CandleData>& candles) const;

    /// Рассчитать только volume score (0-20)
    /// log-шкала: $100K → ~5, $1M → ~10, $10M → ~15, $100M → ~20
    double compute_volume_score(double quote_volume_24h) const;

    /// Рассчитать volatility score (0-20)
    /// Оптимум: 2-5% дневная волатильность → 20 баллов
    /// Слишком низкая (<0.5%) или высокая (>15%) → 0 баллов
    double compute_volatility_score(const std::vector<CandleData>& candles) const;

    /// Рассчитать spread score (0-20)
    /// 0 bps → 20, 10 bps → 15, 30 bps → 5, 50+ bps → 0
    double compute_spread_score(double spread_bps) const;

    /// Рассчитать trend score (0-20)
    /// Упрощённый ADX: сила направленного движения из свечей
    double compute_trend_score(const std::vector<CandleData>& candles) const;

    /// Рассчитать quality score (0-20)
    /// Отношение тела к теням свечей — чистое движение vs шум
    double compute_quality_score(const std::vector<CandleData>& candles) const;

private:
    /// Рассчитать дневную волатильность из часовых свечей
    double compute_daily_volatility(const std::vector<CandleData>& candles) const;

    /// Рассчитать средний body/range ratio свечей
    double compute_body_ratio(const std::vector<CandleData>& candles) const;

    /// Рассчитать упрощённый ADX из свечей (без TA-Lib, на основе DM+/DM-)
    double compute_simple_adx(const std::vector<CandleData>& candles) const;
};

} // namespace tb::pair_scanner
