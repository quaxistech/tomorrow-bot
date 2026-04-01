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

    const auto& micro = ctx.features.microstructure;
    const auto& tech = ctx.features.technical;

    // ─── 1. Emergency: VPIN токсичный → немедленный выход ─────────────────
    if (cfg_.exit_on_trap_detection && micro.vpin_valid && micro.vpin_toxic) {
        result.action = StrategySignalType::EmergencyExit;
        result.exit_reason = ExitReason::TrapDetected;
        result.confidence = 0.95;
        result.reasons.push_back("vpin_toxic_during_position");
        return result;
    }

    // ─── 2. Time stop ────────────────────────────────────────────────────
    if (check_time_stop(pos, now_ns)) {
        result.action = StrategySignalType::ExitFull;
        result.exit_reason = ExitReason::TimeExit;
        result.confidence = 0.85;
        result.reasons.push_back("max_hold_time_exceeded");
        return result;
    }

    // ─── 3. Целевой ход достигнут ────────────────────────────────────────
    if (check_target_reached(pos, ctx)) {
        result.action = StrategySignalType::ExitFull;
        result.exit_reason = ExitReason::TakeProfit;
        result.confidence = 0.90;
        result.reasons.push_back("target_move_reached");
        return result;
    }

    // ─── 4. Структурный провал (тренд сломался) ──────────────────────────
    if (cfg_.exit_on_microtrend_failure && check_structure_failure(pos, ctx)) {
        result.action = StrategySignalType::ExitFull;
        result.exit_reason = ExitReason::TrendFailure;
        result.confidence = 0.80;
        result.reasons.push_back("microtrend_structure_failed");
        return result;
    }

    // ─── 5. Деградация качества → partial reduce ────────────────────────
    if (cfg_.reduce_on_structure_degradation && check_quality_degradation(pos, ctx)) {
        result.action = StrategySignalType::Reduce;
        result.exit_reason = ExitReason::SignalDecay;
        result.reduce_fraction = cfg_.reduce_fraction;
        result.confidence = 0.65;
        result.reasons.push_back("quality_degradation_reduce");
        return result;
    }

    // ─── 6. Trailing stop: значительный откат от пика ────────────────────
    if (pos.peak_pnl_pct > 0.003) {  // Был перевод в прибыль ≥ 0.3%
        double drawdown_from_peak = pos.peak_pnl_pct - pos.unrealized_pnl_pct;
        if (drawdown_from_peak > pos.peak_pnl_pct * 0.6) {  // Потеряли 60% от пиковой прибыли
            result.action = StrategySignalType::ExitFull;
            result.exit_reason = ExitReason::TrailingStop;
            result.confidence = 0.85;
            result.reasons.push_back("trailing_stop_triggered");
            result.reasons.push_back("peak_pnl_pct_" + std::to_string(pos.peak_pnl_pct));
            return result;
        }
    }

    // ─── 7. Стакан развернулся против позиции ────────────────────────────
    if (micro.book_imbalance_valid) {
        bool book_adverse = false;
        if (pos.position_side == PositionSide::Long && micro.book_imbalance_5 < -0.25) {
            book_adverse = true;
        }
        if (pos.position_side == PositionSide::Short && micro.book_imbalance_5 > 0.25) {
            book_adverse = true;
        }
        if (book_adverse && pos.unrealized_pnl_pct < -0.001) {
            result.action = StrategySignalType::ExitFull;
            result.exit_reason = ExitReason::OrderBookDeterioration;
            result.confidence = 0.75;
            result.reasons.push_back("orderbook_turned_against");
            return result;
        }
    }

    // ─── 8. Momentum exhaustion: momentum = 0 при удержании ──────────────
    if (tech.momentum_valid) {
        bool momentum_dead = std::abs(tech.momentum_5) < 0.0003;
        int64_t hold_ms = pos.hold_duration_ns / 1'000'000;
        if (momentum_dead && hold_ms > cfg_.max_hold_time_ms / 3 && pos.unrealized_pnl_pct < 0.001) {
            result.action = StrategySignalType::ExitFull;
            result.exit_reason = ExitReason::MomentumExhaustion;
            result.confidence = 0.70;
            result.reasons.push_back("momentum_exhausted");
            return result;
        }
    }

    // ─── Default: HOLD ───────────────────────────────────────────────────
    result.reasons.push_back("position_stable");
    if (pos.unrealized_pnl_pct > 0.0) {
        result.reasons.push_back("unrealized_profit");
    }

    return result;
}

bool PositionManager::check_time_stop(const StrategyPositionContext& pos, int64_t now_ns) const {
    int64_t hold_ms = pos.hold_duration_ns / 1'000'000;
    return hold_ms >= cfg_.max_hold_time_ms;
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
