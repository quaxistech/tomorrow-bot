#include "risk/policies/risk_checks.hpp"
#include "risk/risk_context.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "order_book/order_book_types.hpp"
#include "common/constants.hpp"
#include "common/enums.hpp"
#include <cmath>
#include <algorithm>

namespace tb::risk {

// Вспомогательные функции
namespace {

void deny(RiskDecision& d, const std::string& code, const std::string& msg,
          double severity = 1.0) {
    d.verdict = RiskVerdict::Denied;
    d.reasons.push_back({code, msg, severity});
    d.hard_blocks.push_back(code);
}

void deny_lock(RiskDecision& d, RiskAction action,
               const std::string& code, const std::string& msg) {
    d.verdict = RiskVerdict::Denied;
    d.action = action;
    d.reasons.push_back({code, msg, 1.0});
    d.hard_blocks.push_back(code);
}

void reduce(RiskDecision& d, double ratio, const std::string& code, const std::string& msg) {
    if (d.verdict == RiskVerdict::Approved)
        d.verdict = RiskVerdict::ReduceSize;
    // Используем min-семантику: каждый reduce вычисляет целевой размер от original_size,
    // итоговый approved_quantity = min(все предложения). Это предотвращает мультипликативное
    // компаундирование нескольких ReduceSize-проверок.
    double target = d.original_size.get() * std::clamp(ratio, 0.0, 1.0);
    if (target < d.approved_quantity.get()) {
        d.approved_quantity = Quantity(target);
    }
    d.reasons.push_back({code, msg, 0.5});
    d.warnings.push_back(code);
}

void throttle(RiskDecision& d, const std::string& code, const std::string& msg) {
    if (d.verdict != RiskVerdict::Denied)
        d.verdict = RiskVerdict::Throttled;
    d.reasons.push_back({code, msg, 0.6});
    d.warnings.push_back(code);
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// 1. Kill Switch
// ═══════════════════════════════════════════════════════════════

void KillSwitchCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    if (state_.locks.has_emergency_halt()) {
        deny_lock(d, RiskAction::EmergencyHalt, "KILL_SWITCH",
                  "Аварийный выключатель активирован");
        d.kill_switch_active = true;
    }
}

// ═══════════════════════════════════════════════════════════════
// 2. Day Lock
// ═══════════════════════════════════════════════════════════════

void DayLockCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    if (state_.locks.has_day_lock()) {
        deny_lock(d, RiskAction::DenyDayLock, "DAY_LOCK",
                  "Торговля заблокирована на сегодня");
    }
}

// ═══════════════════════════════════════════════════════════════
// 3. Symbol Lock
// ═══════════════════════════════════════════════════════════════

void SymbolLockCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (state_.locks.has_symbol_lock(ctx.intent.symbol.get())) {
        deny_lock(d, RiskAction::DenySymbolLock, "SYMBOL_LOCK",
                  "Символ " + ctx.intent.symbol.get() + " заблокирован");
    }
}

// ═══════════════════════════════════════════════════════════════
// 4. Strategy Lock
// ═══════════════════════════════════════════════════════════════

void StrategyLockCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (state_.locks.has_strategy_lock(ctx.intent.strategy_id.get())) {
        deny_lock(d, RiskAction::DenyStrategyLock, "STRATEGY_LOCK",
                  "Стратегия " + ctx.intent.strategy_id.get() + " заблокирована");
    }
}

// ═══════════════════════════════════════════════════════════════
// 5. Symbol Cooldown
// ═══════════════════════════════════════════════════════════════

void SymbolCooldownCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (state_.locks.has_cooldown(ctx.intent.symbol.get())) {
        throttle(d, "SYMBOL_COOLDOWN",
                 "Символ " + ctx.intent.symbol.get() + " в кулдауне");
    }
}

// ═══════════════════════════════════════════════════════════════
// 6. Max Daily Loss
// ═══════════════════════════════════════════════════════════════

void DailyLossCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    const double total_loss = std::min(ctx.portfolio.pnl.total_pnl, 0.0);
    const double loss_pct = std::abs(total_loss) / ctx.portfolio.total_capital * 100.0;
    d.current_daily_pnl = ctx.portfolio.pnl.total_pnl;

    if (loss_pct >= cfg_.max_daily_loss_pct) {
        deny(d, "MAX_DAILY_LOSS",
             "Дневной убыток " + std::to_string(loss_pct) + "% >= лимит " +
             std::to_string(cfg_.max_daily_loss_pct) + "%");
    }
}

// ═══════════════════════════════════════════════════════════════
// 7. Realized Daily Loss
// ═══════════════════════════════════════════════════════════════

void RealizedDailyLossCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    const double realized = std::min(ctx.portfolio.pnl.realized_pnl_today, 0.0);
    const double pct = std::abs(realized) / ctx.portfolio.total_capital * 100.0;

    if (pct >= cfg_.max_realized_daily_loss_pct) {
        deny(d, "REALIZED_DAILY_LOSS",
             "Реализованный дневной убыток " + std::to_string(pct) + "% >= " +
             std::to_string(cfg_.max_realized_daily_loss_pct) + "%");
    }
}

// ═══════════════════════════════════════════════════════════════
// 8. Max Drawdown
// ═══════════════════════════════════════════════════════════════

void MaxDrawdownCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    d.current_drawdown_pct = ctx.portfolio.pnl.current_drawdown_pct;

    if (ctx.portfolio.pnl.current_drawdown_pct >= cfg_.max_drawdown_pct) {
        deny(d, "MAX_DRAWDOWN",
             "Просадка " + std::to_string(ctx.portfolio.pnl.current_drawdown_pct) +
             "% >= " + std::to_string(cfg_.max_drawdown_pct) + "%");
    }
}

// ═══════════════════════════════════════════════════════════════
// 9. Max Positions
// ═══════════════════════════════════════════════════════════════

void MaxPositionsCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    const int count = ctx.portfolio.exposure.open_positions_count;
    if (count >= cfg_.max_concurrent_positions) {
        deny(d, "MAX_POSITIONS",
             "Позиций " + std::to_string(count) + " >= лимит " +
             std::to_string(cfg_.max_concurrent_positions), 0.8);
    }
}

// ═══════════════════════════════════════════════════════════════
// 10. Same Direction Positions
// ═══════════════════════════════════════════════════════════════

void SameDirectionCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    int long_count = 0, short_count = 0;
    for (const auto& pos : ctx.portfolio.positions) {
        if (pos.side == Side::Buy) ++long_count;
        else ++short_count;
    }

    if (ctx.intent.side == Side::Buy && long_count >= cfg_.max_simultaneous_long_positions) {
        deny(d, "MAX_LONG_POSITIONS",
             "Long позиций " + std::to_string(long_count) + " >= " +
             std::to_string(cfg_.max_simultaneous_long_positions), 0.7);
    }
    if (ctx.intent.side == Side::Sell && short_count >= cfg_.max_simultaneous_short_positions) {
        deny(d, "MAX_SHORT_POSITIONS",
             "Short позиций " + std::to_string(short_count) + " >= " +
             std::to_string(cfg_.max_simultaneous_short_positions), 0.7);
    }

    // Legacy same_direction check
    int same_dir = (ctx.intent.side == Side::Buy) ? long_count : short_count;
    if (same_dir >= cfg_.max_same_direction_positions) {
        deny(d, "SAME_DIRECTION",
             "Позиций в одном направлении " + std::to_string(same_dir) +
             " >= " + std::to_string(cfg_.max_same_direction_positions), 0.7);
    }
}

// ═══════════════════════════════════════════════════════════════
// 11. Exposure Limit
// ═══════════════════════════════════════════════════════════════

void ExposureLimitCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    // Проецируем post-trade экспозицию: текущая + номинал нового ордера
    const double projected_gross = ctx.portfolio.exposure.gross_exposure +
                                   ctx.sizing.approved_notional.get();
    const double gross_pct = projected_gross / ctx.portfolio.total_capital * 100.0;
    d.current_gross_exposure = ctx.portfolio.exposure.gross_exposure;

    if (gross_pct >= cfg_.max_gross_exposure_pct) {
        deny(d, "MAX_EXPOSURE",
             "Проецируемая экспозиция " + std::to_string(gross_pct) + "% >= " +
             std::to_string(cfg_.max_gross_exposure_pct) + "%", 0.9);
    }
}

// ═══════════════════════════════════════════════════════════════
// 12. Per-Trade Risk
// ═══════════════════════════════════════════════════════════════

void PerTradeRiskCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    const double notional = ctx.sizing.approved_notional.get();

    if (notional > cfg_.max_position_notional) {
        const double ratio = cfg_.max_position_notional / notional;
        reduce(d, ratio, "MAX_NOTIONAL",
               "Номинал " + std::to_string(notional) + " > лимит " +
               std::to_string(cfg_.max_position_notional));
    }
}

// ═══════════════════════════════════════════════════════════════
// 13. Max Leverage
// ═══════════════════════════════════════════════════════════════

void MaxLeverageCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    // Проецируем post-trade leverage: (текущая экспозиция + новый ордер) / капитал
    const double projected_exposure = ctx.portfolio.exposure.gross_exposure +
                                      ctx.sizing.approved_notional.get();
    const double leverage = projected_exposure / ctx.portfolio.total_capital;
    double buffer_factor = 1.0 - (cfg_.liquidation_buffer_pct / 100.0);
    double effective_max = cfg_.max_leverage * buffer_factor;

    if (leverage >= effective_max) {
        deny(d, "MAX_LEVERAGE",
             "Проецируемое плечо " + std::to_string(leverage) + "x >= effective max " +
             std::to_string(effective_max) + "x (buffer " +
             std::to_string(cfg_.liquidation_buffer_pct) + "%)", 0.9);
    }
}

// ═══════════════════════════════════════════════════════════════
// 14. Max Slippage
// ═══════════════════════════════════════════════════════════════

void MaxSlippageCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.exec_alpha.quality.estimated_slippage_bps > cfg_.max_slippage_bps) {
        deny(d, "MAX_SLIPPAGE",
             "Проскальзывание " + std::to_string(ctx.exec_alpha.quality.estimated_slippage_bps) +
             "бп > " + std::to_string(cfg_.max_slippage_bps) + "бп", 0.7);
    }
}

// ═══════════════════════════════════════════════════════════════
// 15. Consecutive Losses
// ═══════════════════════════════════════════════════════════════

void ConsecutiveLossesCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    // Из портфолио
    const int losses = ctx.portfolio.pnl.consecutive_losses;
    if (losses >= cfg_.max_consecutive_losses) {
        deny(d, "CONSECUTIVE_LOSSES",
             "Серия убытков " + std::to_string(losses) + " >= " +
             std::to_string(cfg_.max_consecutive_losses), 0.8);
    }

    // Проверка halt после N убытков
    if (losses >= cfg_.halt_after_n_losses) {
        deny_lock(d, RiskAction::DenyAccountLock, "HALT_AFTER_LOSSES",
                  "Серия убытков " + std::to_string(losses) + " >= halt порог " +
                  std::to_string(cfg_.halt_after_n_losses));
    }
}

// ═══════════════════════════════════════════════════════════════
// 16. Per-Symbol Risk
// ═══════════════════════════════════════════════════════════════

void PerSymbolRiskCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    const auto& symbol = ctx.intent.symbol.get();

    // Концентрация символа (projected: существующая + предлагаемый ордер)
    double symbol_exposure = 0.0;
    for (const auto& pos : ctx.portfolio.positions) {
        if (pos.symbol.get() == symbol) symbol_exposure += pos.notional.get();
    }
    const double projected_exposure = symbol_exposure + ctx.sizing.approved_notional.get();
    const double conc_pct = projected_exposure / ctx.portfolio.total_capital * 100.0;
    d.symbol_concentration_pct = conc_pct;

    if (conc_pct >= cfg_.max_symbol_concentration_pct) {
        deny(d, "SYMBOL_CONCENTRATION",
             "Концентрация " + symbol + " = " + std::to_string(conc_pct) +
             "% >= " + std::to_string(cfg_.max_symbol_concentration_pct) + "%", 0.8);
    }

    // Consecutive losses per symbol
    int sym_losses = state_.loss_streaks.symbol_consecutive_losses(symbol);
    if (sym_losses >= cfg_.max_consecutive_losses_per_symbol) {
        deny(d, "SYMBOL_CONSECUTIVE_LOSSES",
             "Серия убытков по " + symbol + ": " + std::to_string(sym_losses) +
             " >= " + std::to_string(cfg_.max_consecutive_losses_per_symbol), 0.8);
    }

    // Daily loss per symbol
    double sym_pnl = state_.pnl.symbol_daily_pnl(symbol);
    if (sym_pnl < 0.0) {
        double sym_loss_pct = std::abs(sym_pnl) / ctx.portfolio.total_capital * 100.0;
        if (sym_loss_pct >= cfg_.max_daily_loss_per_symbol_pct) {
            deny(d, "SYMBOL_DAILY_LOSS",
                 "Дневной убыток по " + symbol + ": " + std::to_string(sym_loss_pct) +
                 "% >= " + std::to_string(cfg_.max_daily_loss_per_symbol_pct) + "%", 0.8);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// 17. Per-Strategy Risk
// ═══════════════════════════════════════════════════════════════

void PerStrategyRiskCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    const auto& sid = ctx.intent.strategy_id.get();
    auto it = state_.strategy_budgets.find(sid);
    if (it == state_.strategy_budgets.end()) return;

    const auto& budget = it->second;
    const double loss_pct = budget.daily_loss / ctx.portfolio.total_capital * 100.0;
    d.strategy_budget_utilization_pct = loss_pct;

    if (loss_pct >= cfg_.max_strategy_daily_loss_pct) {
        deny_lock(d, RiskAction::DenyStrategyLock, "STRATEGY_BUDGET",
                  "Стратегия " + sid + " потеряла " + std::to_string(loss_pct) +
                  "% >= " + std::to_string(cfg_.max_strategy_daily_loss_pct) + "%");
    }
}

// ═══════════════════════════════════════════════════════════════
// 18. Order Rate
// ═══════════════════════════════════════════════════════════════

void OrderRateCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    int count = state_.rates.orders_last_minute(clock_->now());
    if (count >= cfg_.max_orders_per_minute) {
        throttle(d, "ORDER_RATE",
                 "Ордеров/мин " + std::to_string(count) + " >= " +
                 std::to_string(cfg_.max_orders_per_minute));
    }
}

// ═══════════════════════════════════════════════════════════════
// 19. Turnover Rate
// ═══════════════════════════════════════════════════════════════

void TurnoverRateCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    int count = state_.rates.trades_last_hour(clock_->now());
    if (count >= cfg_.max_trades_per_hour) {
        throttle(d, "TURNOVER_RATE",
                 "Сделок/час " + std::to_string(count) + " >= " +
                 std::to_string(cfg_.max_trades_per_hour));
    }
}

// ═══════════════════════════════════════════════════════════════
// 20. Trade Interval
// ═══════════════════════════════════════════════════════════════

void TradeIntervalCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    int64_t last = state_.rates.last_trade_for_symbol(ctx.intent.symbol.get());
    if (last == 0) return;

    int64_t elapsed = clock_->now().get() - last;
    if (elapsed < cfg_.min_trade_interval_ns) {
        throttle(d, "TRADE_INTERVAL",
                 "Интервал " + std::to_string(elapsed / 1'000'000'000LL) +
                 "с < " + std::to_string(cfg_.min_trade_interval_ns / 1'000'000'000LL) + "с");
    }
}

// ═══════════════════════════════════════════════════════════════
// 21. Stale Feed
// ═══════════════════════════════════════════════════════════════

void StaleFeedCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (!ctx.features.execution_context.is_feed_fresh) {
        deny(d, "STALE_FEED", "Данные рынка устарели");
        return;
    }
    if (ctx.features.market_data_age_ns.get() > cfg_.max_feed_age_ns) {
        deny(d, "STALE_FEED",
             "Возраст данных " + std::to_string(ctx.features.market_data_age_ns.get() / 1'000'000'000LL) +
             "с > " + std::to_string(cfg_.max_feed_age_ns / 1'000'000'000LL) + "с");
    }
}

// ═══════════════════════════════════════════════════════════════
// 22. Book Quality
// ═══════════════════════════════════════════════════════════════

void BookQualityCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.features.book_quality != order_book::BookQuality::Valid) {
        deny(d, "INVALID_BOOK", "Стакан ордеров невалиден");
    }
}

// ═══════════════════════════════════════════════════════════════
// 23. Spread
// ═══════════════════════════════════════════════════════════════

void SpreadCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (!ctx.features.microstructure.spread_valid) return;
    if (ctx.features.microstructure.spread_bps > cfg_.max_spread_bps) {
        deny(d, "WIDE_SPREAD",
             "Спред " + std::to_string(ctx.features.microstructure.spread_bps) +
             "бп > " + std::to_string(cfg_.max_spread_bps) + "бп", 0.7);
    }
}

// ═══════════════════════════════════════════════════════════════
// 24. Liquidity
// ═══════════════════════════════════════════════════════════════

void LiquidityCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (!ctx.features.microstructure.liquidity_valid) return;
    const double total = ctx.features.microstructure.bid_depth_5_notional +
                         ctx.features.microstructure.ask_depth_5_notional;
    if (total < cfg_.min_liquidity_depth) {
        deny(d, "LOW_LIQUIDITY",
             "Ликвидность " + std::to_string(total) + " < " +
             std::to_string(cfg_.min_liquidity_depth), 0.6);
    }
}

// ═══════════════════════════════════════════════════════════════
// 25. Max Loss Per Trade
// ═══════════════════════════════════════════════════════════════

void MaxLossPerTradeCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.total_capital <= 0.0) return;

    for (const auto& pos : ctx.portfolio.positions) {
        if (pos.unrealized_pnl >= 0.0) continue;
        double loss_pct = std::abs(pos.unrealized_pnl) / ctx.portfolio.total_capital * 100.0;
        if (loss_pct >= cfg_.max_loss_per_trade_pct) {
            deny(d, "MAX_LOSS_PER_TRADE",
                 "Позиция " + pos.symbol.get() + " теряет " + std::to_string(loss_pct) +
                 "% капитала (лимит: " + std::to_string(cfg_.max_loss_per_trade_pct) + "%)", 0.9);
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// 26. UTC Cutoff
// ═══════════════════════════════════════════════════════════════

void UtcCutoffCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    if (cfg_.utc_cutoff_hour < 0) return;

    const int64_t now_ns = clock_->now().get();
    const int hour_utc = static_cast<int>((now_ns / 1'000'000'000LL % 86400) / 3600);
    if (hour_utc >= cfg_.utc_cutoff_hour) {
        deny(d, "UTC_CUTOFF",
             "Торговля прекращена после " + std::to_string(cfg_.utc_cutoff_hour) + ":00 UTC");
    }
}

// ═══════════════════════════════════════════════════════════════
// 27. Regime Scaled Limits
// ═══════════════════════════════════════════════════════════════

void RegimeScaledLimitsCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    const double scale = regime_scale_.load(std::memory_order_acquire);
    d.regime_scaling_factor = scale;

    if (!cfg_.regime_aware_limits_enabled || scale >= 1.0) return;

    const double scaled_max = cfg_.max_position_notional * scale;
    const double notional = ctx.sizing.approved_notional.get();
    if (notional <= scaled_max) return;

    const double ratio = scaled_max / notional;
    reduce(d, ratio, "REGIME_SCALED_LIMIT",
           "Режим scale=" + std::to_string(scale) + " — макс. номинал " +
           std::to_string(scaled_max));
}

// ═══════════════════════════════════════════════════════════════
// 28. Uncertainty Limits
// ═══════════════════════════════════════════════════════════════

void UncertaintyLimitsCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.uncertainty.level != UncertaintyLevel::High &&
        ctx.uncertainty.level != UncertaintyLevel::Extreme) return;

    const double adj_max = cfg_.max_position_notional * ctx.uncertainty.size_multiplier;
    const double notional = ctx.sizing.approved_notional.get();
    if (notional <= adj_max) return;

    const double ratio = adj_max / notional;
    reduce(d, ratio, "UNCERTAINTY_LIMIT",
           "Неопределённость " + std::string(to_string(ctx.uncertainty.level)) +
           " — mult=" + std::to_string(ctx.uncertainty.size_multiplier));
}

// ═══════════════════════════════════════════════════════════════
// 29. Uncertainty Cooldown
// ═══════════════════════════════════════════════════════════════

void UncertaintyCooldownCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (!ctx.uncertainty.cooldown.active) return;
    throttle(d, "UNCERTAINTY_COOLDOWN",
             "Кулдаун неопр.: " + ctx.uncertainty.cooldown.trigger_reason);
}

// ═══════════════════════════════════════════════════════════════
// 30. Uncertainty Execution Mode
// ═══════════════════════════════════════════════════════════════

void UncertaintyExecutionModeCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.uncertainty.execution_mode != uncertainty::ExecutionModeRecommendation::HaltNewEntries)
        return;
    if (ctx.intent.trade_side == TradeSide::Open) {
        deny(d, "UNCERTAINTY_HALT", "HaltNewEntries — новые входы запрещены");
    }
}

// ═══════════════════════════════════════════════════════════════
// 31. Intraday Drawdown
// ═══════════════════════════════════════════════════════════════

void IntradayDrawdownCheck::evaluate(const RiskContext& /*ctx*/, RiskDecision& d) {
    double dd = state_.drawdown.intraday_drawdown_pct();
    if (dd >= cfg_.max_intraday_drawdown_pct) {
        deny(d, "INTRADAY_DRAWDOWN",
             "Внутридневная просадка " + std::to_string(dd) + "% >= " +
             std::to_string(cfg_.max_intraday_drawdown_pct) + "%", 0.9);
    }
}

// ═══════════════════════════════════════════════════════════════
// 32. Drawdown Hard Stop
// ═══════════════════════════════════════════════════════════════

void DrawdownHardStopCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    if (ctx.portfolio.pnl.current_drawdown_pct >= cfg_.drawdown_hard_stop_pct) {
        deny_lock(d, RiskAction::EmergencyHalt, "DRAWDOWN_HARD_STOP",
                  "Критическая просадка " + std::to_string(ctx.portfolio.pnl.current_drawdown_pct) +
                  "% >= " + std::to_string(cfg_.drawdown_hard_stop_pct) + "%");
        // Активируем emergency halt
        state_.locks.add_lock(LockType::EmergencyHalt, "", "Drawdown hard stop",
                              d.decided_at);
    }
}

// ═══════════════════════════════════════════════════════════════
// 33. Funding Rate Cost
// ═══════════════════════════════════════════════════════════════

void FundingRateCostCheck::evaluate(const RiskContext& ctx, RiskDecision& d) {
    double rate = ctx.current_funding_rate;
    if (std::abs(rate) < 1e-9) return;  // Нет данных или нулевой rate

    // Funding cost depends on position direction:
    // Long pays when rate > 0, receives when rate < 0
    // Short pays when rate < 0, receives when rate > 0
    bool is_long = (ctx.intent.side == Side::Buy);
    double effective_cost = is_long ? rate : -rate;

    // If funding is favorable (effective_cost <= 0), no check needed
    if (effective_cost <= 0.0) return;

    // Годовая стоимость: cost × 3 сеанса/день × 365 дней × 100 (%)
    double annual_cost_pct = effective_cost * 3.0 * 365.0 * 100.0;
    if (annual_cost_pct > cfg_.max_annual_funding_cost_pct) {
        deny(d, "FUNDING_COST_EXCESSIVE",
             "Годовая стоимость фандинга " + std::to_string(annual_cost_pct) +
             "% > лимит " + std::to_string(cfg_.max_annual_funding_cost_pct) + "%",
             0.7);
    }
}

} // namespace tb::risk
