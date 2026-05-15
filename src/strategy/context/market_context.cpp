#include "strategy/context/market_context.hpp"
#include <cmath>

namespace tb::strategy {

MarketContextResult MarketContextEvaluator::evaluate(const StrategyContext& ctx) const {
    MarketContextResult result;
    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;
    const auto& exec = ctx.features.execution_context;

    // 1. Проверка свежести данных (§28)
    result.freshness_ok = ctx.data_fresh && exec.is_feed_fresh;
    if (!result.freshness_ok) {
        result.quality = MarketContextQuality::Invalid;
        result.reasons.push_back("stale_data");
        return result;
    }

    // 2. Проверка валидности микроструктурных данных
    if (!micro.spread_valid || !micro.book_imbalance_valid) {
        result.quality = MarketContextQuality::Invalid;
        result.reasons.push_back("microstructure_data_invalid");
        return result;
    }

    // 3. Проверка спреда
    // BUG-S31-03: negative spread_bps (bid > ask) means corrupted book data.
    // Any negative value would pass "<= max_spread_bps" silently.
    if (micro.spread_bps < 0.0) {
        result.quality = MarketContextQuality::Invalid;
        result.reasons.push_back("negative_spread_corrupted_book");
        return result;
    }
    result.spread_ok = micro.spread_bps <= cfg_.max_spread_bps_for_entry;
    if (!result.spread_ok) {
        result.reasons.push_back("spread_too_wide");
    }

    // 4. Проверка ликвидности
    if (micro.liquidity_valid) {
        result.liquidity_ok = micro.liquidity_ratio >= cfg_.min_liquidity_ratio;
    } else {
        // Если данные ликвидности недоступны, используем косвенную оценку
        result.liquidity_ok = (micro.bid_depth_5_notional > 0.0 && micro.ask_depth_5_notional > 0.0);
    }
    if (!result.liquidity_ok) {
        result.reasons.push_back("low_liquidity");
    }

    // 5. VPIN токсичность
    if (micro.vpin_valid) {
        result.vpin_ok = !micro.vpin_toxic;
        if (!result.vpin_ok) {
            result.reasons.push_back("vpin_toxic_flow");
        }
    }

    // 6. Волатильность (слишком высокая = опасно, слишком низкая = нет возможности)
    if (tech.volatility_valid) {
        // Аномальная волатильность — красный флаг
        double vol_ratio = (tech.volatility_20 > 0.0) ?
                           tech.volatility_5 / tech.volatility_20 : 1.0;
        result.volatility_ok = (vol_ratio < 3.0) && (tech.volatility_5 > 0.0001);
        if (!result.volatility_ok) {
            result.reasons.push_back(vol_ratio >= 3.0 ? "extreme_volatility" : "zero_volatility");
        }
    }

    // 7. Определяем интегральное качество
    int failed = 0;
    if (!result.spread_ok) ++failed;
    if (!result.liquidity_ok) ++failed;
    if (!result.vpin_ok) ++failed;
    if (!result.volatility_ok) ++failed;

    if (failed == 0) {
        // Отличные условия: узкий спред + высокая ликвидность + низкий VPIN
        bool tight_spread = micro.spread_bps < cfg_.max_spread_bps_for_entry * 0.5;
        bool deep_book = micro.liquidity_ratio > cfg_.min_liquidity_ratio * 2.0;
        result.quality = (tight_spread && deep_book) ?
                         MarketContextQuality::Excellent : MarketContextQuality::Good;
        result.reasons.push_back("context_favorable");
    } else if (failed == 1) {
        result.quality = MarketContextQuality::Marginal;
    } else {
        result.quality = MarketContextQuality::Poor;
    }

    return result;
}

} // namespace tb::strategy
