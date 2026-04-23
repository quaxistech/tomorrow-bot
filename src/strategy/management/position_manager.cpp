#include "strategy/management/position_manager.hpp"
#include <cmath>
#include <algorithm>

namespace tb::strategy {

PositionManagementResult PositionManager::evaluate(const StrategyPositionContext& pos,
                                                    const StrategyContext& ctx,
                                                    int64_t now_ns) const {
    PositionManagementResult result;
    result.action = StrategySignalType::Hold;
    result.confidence = pos.entry_confidence;

    if (!pos.has_position) {
        result.action = StrategySignalType::Skip;
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Все exit/reduce решения централизованы в pipeline::PositionExitOrchestrator.
    // PositionManager НЕ принимает exit-решения — это единый владелец (Phase 2).
    // Ранее здесь были: VPIN toxic, TakeProfit, TrendFailure, SignalDecay,
    // OrderBookDeterioration. Все перенесены в exit_orchestrator.
    // ═══════════════════════════════════════════════════════════════════════

    result.reasons.push_back("position_stable");
    if (pos.unrealized_pnl_pct > 0.0) {
        result.reasons.push_back("unrealized_profit");
    }

    return result;
}

bool PositionManager::check_structure_failure(const StrategyPositionContext& pos,
                                               const StrategyContext& ctx) const {
    const auto& tech = ctx.features.technical;
    if (!tech.ema_valid) return false;

    // Структурный провал: EMA тренд развернулся
    if (pos.position_side == PositionSide::Long) {
        return tech.ema_20 < tech.ema_50 && tech.momentum_valid && tech.momentum_5 < -0.002;
    } else {
        return tech.ema_20 > tech.ema_50 && tech.momentum_valid && tech.momentum_5 > 0.002;
    }
}

bool PositionManager::check_target_reached(const StrategyPositionContext& pos,
                                            const StrategyContext& ctx) const {
    // Цель: движение в 2x ATR от входа
    if (pos.entry_atr <= 0.0 || pos.avg_entry_price <= 0.0) return false;

    double target_move_pct = (pos.entry_atr * 2.0) / pos.avg_entry_price;
    return pos.unrealized_pnl_pct >= target_move_pct;
}

bool PositionManager::check_quality_degradation(const StrategyPositionContext& pos,
                                                 const StrategyContext& ctx) const {
    const auto& micro = ctx.features.microstructure;

    // Спред значительно вырос от момента входа
    if (micro.spread_valid && micro.spread_bps > cfg_.max_spread_bps_for_entry * 1.3) {
        return true;
    }

    // Ликвидность сильно упала
    if (micro.liquidity_valid && micro.liquidity_ratio < cfg_.min_liquidity_ratio * 0.7) {
        return true;
    }

    return false;
}

} // namespace tb::strategy
