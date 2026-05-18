#include "pipeline/exit_orchestrator.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace tb::pipeline {

PositionExitOrchestrator::PositionExitOrchestrator(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock)) {}

// ─── Main evaluate: runs all exit signals in priority order ───────────────

// edge-31 Phase 5: Simplified two-layer exit architecture.
//
// Layer 1 (exchange-native protection): TP/SL prikrep'ленные к entry order через
//   AttachedTpSl (Phase 1) + ProtectiveBracketManager (Phase 3). Эти брекеты
//   живут на бирже и срабатывают независимо от состояния бота.
// Layer 2 (adaptive management): TrailingManager пушит обновлённый SL на биржу
//   через bracket_manager_->update_sl() (Phase 4). Локальный close по trailing
//   breach больше НЕ инициируется.
//
// Этот orchestrator теперь отвечает ТОЛЬКО за emergency-tier safety net'ы,
// которые обходят обычный exit-path:
//   1) fixed_capital_stop — последняя защита от catastrophic loss
//   2) toxic_flow — токсичная микроструктура: вылет до того как book опустошится
//   3) stale_data — нельзя торговать вслепую: pull plug при rotting feed
//
// Все ранее использовавшиеся "alpha-tier" exits (continuation_value, structural_failure,
// market_regime_exit, liquidity_deterioration, partial_tp, quick_profit,
// fast_adverse_tiered, funding_carry, price_stop, force_tp_high) удалены — их
// функцию закрывает TP+SL+trailing на бирже. См. docs/refactor_plan.md (edge-31).
ExitDecision PositionExitOrchestrator::evaluate(const ExitContext& ctx) const {
    if (ctx.hedge_active) {
        // Hedge is managed separately — orchestrator doesn't override hedge manager
        return {};
    }

    // 1. Hard capital stop — последний рубеж safety. Если по какой-то причине
    //    exchange SL не сработал (race, API failure), портфельный capital-loss
    //    cap отрабатывает локально через market close.
    if (auto d = check_fixed_capital_stop(ctx); d.should_exit) return d;

    // 2. Toxic flow — emergency. Микроструктура развалилась → вылет до того как
    //    book опустошится и slippage станет неуправляемым.
    if (auto d = check_toxic_flow(ctx); d.should_exit) return d;

    // 3. Stale data — emergency. Feed протух → нельзя торговать вслепую.
    if (auto d = check_stale_data_exit(ctx); d.should_exit) return d;

    // Всё остальное — на биржевых TP/SL и trailing. Locale больше ничего не
    // инициирует, чтобы не дублировать exchange-native protection.
    return {};
}

// ─── Hard stops ───────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_fixed_capital_stop(const ExitContext& ctx) const {
    if (ctx.total_capital <= 0.0) return {};
    double loss_pct = std::abs(std::min(ctx.unrealized_pnl, 0.0)) / ctx.total_capital * 100.0;
    if (loss_pct < ctx.max_loss_per_trade_pct) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 1.0;
    d.explanation = build_explanation(
        ExitSignalType::HardRiskStop,
        std::format("Capital loss {:.2f}% >= {:.2f}% limit", loss_pct, ctx.max_loss_per_trade_pct),
        {},
        "Position would need to recover to reduce loss below capital limit");
    return d;
}

// edge-31 Phase 5: check_price_stop удалён — exchange-attached SL (presetStopLoss)
// плюс ProtectiveBracketManager делают локальный price-based exit избыточным.
//
// edge-31 Phase 5: check_trailing_stop удалён — trailing теперь не закрывает
// позицию локально, а пушит обновлённый SL на биржу через
// bracket_manager_->update_sl() (см. update_trailing_stop в trading_pipeline.cpp,
// Phase 4). update_trailing ниже сохраняется — она вычисляет новый SL level
// для этого пуша.

PositionExitOrchestrator::TrailingUpdate
PositionExitOrchestrator::update_trailing(const ExitContext& ctx) const {
    TrailingUpdate u;
    u.stop_level = ctx.current_stop_level;
    u.trail_mult = ctx.current_trail_mult;
    u.breakeven_activated = ctx.breakeven_activated;
    u.highest = ctx.highest_price_since_entry;
    u.lowest = ctx.lowest_price_since_entry;

    if (!ctx.atr_valid || ctx.atr_14 <= 0.0 || ctx.entry_price <= 0.0) return u;

    double atr = ctx.atr_14;
    double entry = ctx.entry_price;
    double price = ctx.current_price;
    bool is_long = (ctx.position_side == PositionSide::Long);

    // Dynamic ATR multiplier
    double base = ctx.atr_stop_multiplier;

    // B37.1: empirical thresholds для динамической ATR-multiplier калибровки.
    // Эти значения формируют корзину контекста (trend / depth / spread / vol);
    // вынести в TrailingConfig если потребуется per-market калибровка.
    constexpr double kAdxStrongThr = 30.0, kAdxWeakThr = 20.0;
    constexpr double kAdxStrongMult = 0.85, kAdxWeakMult = 1.15;
    constexpr double kDepthThin = 500.0, kDepthMid = 2000.0;
    constexpr double kDepthThinMult = 1.3, kDepthMidMult = 1.1;
    constexpr double kSpreadWideThr = 50.0, kSpreadMidThr = 20.0;
    constexpr double kSpreadWideMult = 1.3, kSpreadMidMult = 1.1;
    constexpr double kBbWideThr = 0.05, kBbMidThr = 0.03;
    constexpr double kBbWideMult = 1.2, kBbMidMult = 1.1;

    double adx_f = 1.0;
    if (ctx.adx > kAdxStrongThr) adx_f = kAdxStrongMult;
    else if (ctx.adx <= kAdxWeakThr) adx_f = kAdxWeakMult;

    double depth_f = 1.0;
    if (ctx.depth_usd < kDepthThin) depth_f = kDepthThinMult;
    else if (ctx.depth_usd < kDepthMid) depth_f = kDepthMidMult;

    double spread_f = 1.0;
    if (ctx.spread_bps > kSpreadWideThr) spread_f = kSpreadWideMult;
    else if (ctx.spread_bps > kSpreadMidThr) spread_f = kSpreadMidMult;

    double bb_f = 1.0;
    if (ctx.bb_width > kBbWideThr) bb_f = kBbWideMult;
    else if (ctx.bb_width > kBbMidThr) bb_f = kBbMidMult;

    double pressure_f = 1.0;
    if (is_long && ctx.buy_pressure < 0.3) pressure_f = 0.8;
    if (!is_long && ctx.buy_pressure > 0.7) pressure_f = 0.8;

    // EDGE-30-EXIT (run64 2026-05-15): wider trail когда уже в profit — let winners run.
    // User explicit: максимальная прибыль при profitable trade.
    // Scale trail_mult by profit_in_atr: 0 profit → base, 2×ATR profit → 2× base.
    double profit_f = 1.0;
    if (ctx.atr_14 > 0.0 && ctx.entry_price > 0.0) {
        double profit_in_atr = is_long
            ? (price - entry) / ctx.atr_14
            : (entry - price) / ctx.atr_14;
        if (profit_in_atr > 0.5) {
            // Linear scale: 0.5 ATR → 1.0×, 2.0 ATR → 1.8×, capped
            profit_f = std::min(1.8, 1.0 + (profit_in_atr - 0.5) * 0.5);
        }
    }

    u.trail_mult = std::clamp(
        base * adx_f * depth_f * spread_f * bb_f * pressure_f * profit_f,
        0.8, 5.0);

    double min_fee_offset = entry * ctx.taker_fee_pct / 100.0 * 3.0;
    double fee_floor_in_atr = (atr > 0.0) ? (min_fee_offset / atr) : 100.0;
    double effective_be_threshold = std::max(ctx.breakeven_atr_threshold, fee_floor_in_atr * 1.5);

    if (is_long) {
        u.highest = std::max(ctx.highest_price_since_entry, price);
        double profit_in_atr = (price - entry) / atr;

        if (!ctx.breakeven_activated) {
            if (profit_in_atr >= effective_be_threshold) {
                u.breakeven_activated = true;
                double current_profit = price - entry;
                double be = entry + current_profit * 0.5;
                be = std::max(be, entry + min_fee_offset);
                be = std::max(be, entry + atr * 0.1);
                be = std::min(be, price - atr * 0.1);
                u.stop_level = be;
            } else if (u.stop_level <= 0.0) {
                double dist = std::max(u.trail_mult * atr, entry * 0.003);
                u.stop_level = entry - dist;
            }
        } else {
            double dist = std::max(u.trail_mult * atr, entry * 0.003);
            double new_stop = u.highest - dist;
            u.stop_level = std::max(ctx.current_stop_level, new_stop);
        }
    } else {
        u.lowest = std::min(ctx.lowest_price_since_entry, price);
        double profit_in_atr = (entry - price) / atr;

        if (!ctx.breakeven_activated) {
            if (profit_in_atr >= effective_be_threshold) {
                u.breakeven_activated = true;
                double current_profit = entry - price;
                double be = entry - current_profit * 0.5;
                be = std::min(be, entry - min_fee_offset);
                be = std::min(be, entry - atr * 0.1);
                be = std::max(be, price + atr * 0.1);
                u.stop_level = be;
            } else if (u.stop_level <= 0.0) {
                double dist = std::max(u.trail_mult * atr, entry * 0.003);
                u.stop_level = entry + dist;
            }
        } else {
            double dist = std::max(u.trail_mult * atr, entry * 0.003);
            double new_stop = u.lowest + dist;
            if (ctx.current_stop_level <= 0.0) {
                u.stop_level = new_stop;
            } else {
                u.stop_level = std::min(ctx.current_stop_level, new_stop);
            }
        }
    }

    return u;
}

// edge-31 Phase 5: check_quick_profit и check_partial_tp удалены — exchange-attached
// TP закрывает позицию атомарно когда цель достигнута. Partial-TP создавал шум
// (преждевременные reduce ордера на low-net-RR setups). См. docs/refactor_plan.md.

// ─── Toxic flow ────────────────────────────────────────────────────────────

ExitDecision PositionExitOrchestrator::check_toxic_flow(const ExitContext& ctx) const {
    if (!ctx.vpin_toxic) return {};
    // EDGE-24 (run42 evidence): VPIN flickers toxic на alts с low volume.
    // Раньше exit при ЛЮБОЙ потере + VPIN — 3/4 trades закрывались на near-zero pnl.
    // Теперь требуем meaningful loss (>0.3%) ИЛИ extended hold time (>5min).
    if (ctx.unrealized_pnl_pct >= -0.3) return {};
    int64_t hold_ns = ctx.now_ns - ctx.entry_time_ns;
    if (hold_ns < 300LL * 1'000'000'000LL && ctx.unrealized_pnl_pct >= -0.5) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.95;
    d.explanation = build_explanation(
        ExitSignalType::ToxicFlowExit,
        "VPIN toxic flow + meaningful loss (EDGE-24 threshold)",
        {},
        "Would hold if position above -0.3% or VPIN not toxic");
    return d;
}

// edge-31 Phase 5: check_structural_failure, check_liquidity_deterioration,
// check_market_regime_exit, check_funding_carry_exit удалены —
// эти "alpha-tier" exits перекрывались с trailing SL и net-RR гейтом на entry.
// EMA-cross / regime change теперь делают signal-based exit через стратегию,
// а deteriorating liquidity и adverse funding отсекаются в NetRRGate (Phase 2)
// на этапе входа. Подробности — docs/refactor_plan.md (edge-31).

// ─── Stale Data Exit ───────────────────────────────────────────────────────
// Exits when market data feed is not fresh — cannot make informed decisions.

ExitDecision PositionExitOrchestrator::check_stale_data_exit(const ExitContext& ctx) const {
    if (ctx.is_feed_fresh) return {};

    // Only exit if position is at risk (in loss or thin market)
    bool at_risk = ctx.unrealized_pnl_pct < -0.5 || ctx.depth_usd < 1000.0;
    if (!at_risk) return {};

    ExitDecision d;
    d.should_exit = true;
    d.urgency = 0.85;
    d.state = {};
    d.explanation.primary_signal = ExitSignalType::StaleDataExit;
    d.explanation.category = ExitCategory::StaleData;
    d.explanation.primary_driver = std::format(
        "Stale data exit: feed not fresh, PnL={:.2f}%, depth=${:.0f}",
        ctx.unrealized_pnl_pct, ctx.depth_usd);
    d.explanation.counterfactual = "Would hold if feed were fresh or position were profitable";
    return d;
}

// ─── Explanation builder ───────────────────────────────────────────────────

ExitExplanation PositionExitOrchestrator::build_explanation(
    ExitSignalType signal,
    const std::string& primary,
    const ContinuationState& state,
    const std::string& counterfactual) const {

    ExitExplanation e;
    e.primary_signal = signal;
    e.category = exit_category(signal);
    e.primary_driver = primary;
    e.counterfactual = counterfactual;

    // Add state components as secondary drivers if state was computed
    if (state.continuation_value != 0.0 || state.trend_persistence != 0.0) {
        if (std::abs(state.trend_persistence) > 0.01)
            e.secondary_drivers.push_back({"trend_persistence", state.trend_persistence});
        if (state.edge_decay > 0.01)
            e.secondary_drivers.push_back({"edge_decay", state.edge_decay});
        if (state.toxic_flow_score > 0.01)
            e.secondary_drivers.push_back({"toxic_flow", state.toxic_flow_score});
    }

    return e;
}

} // namespace tb::pipeline
