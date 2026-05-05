#include "risk/state/risk_state.hpp"
#include <algorithm>
#include <cmath>

namespace tb::risk {

// ═══════════════════════════════════════════════════════════════
// LockRegistry
// ═══════════════════════════════════════════════════════════════

void LockRegistry::add_lock(LockType type, const std::string& target,
                            const std::string& reason, Timestamp now, int64_t duration_ns) {
    // BUG-RS-07 fix: if the lock already exists, do NOT reset its timer.
    // Resetting the timer on each new loss would create perpetual cooldown — every
    // additional loss restarts the cooldown window so it never expires.
    // Only add the lock if it is not already present.
    for (const auto& l : locks_) {
        if (l.type == type && l.target == target) {
            return; // Lock already active; preserve original activation time.
        }
    }
    locks_.push_back({type, target, reason, now, duration_ns});
}

void LockRegistry::remove_lock(LockType type, const std::string& target) {
    locks_.erase(
        std::remove_if(locks_.begin(), locks_.end(),
                       [&](const LockRecord& l) { return l.type == type && l.target == target; }),
        locks_.end());
}

void LockRegistry::clear_expired(Timestamp now) {
    locks_.erase(
        std::remove_if(locks_.begin(), locks_.end(),
                       [&](const LockRecord& l) {
                           if (l.duration_ns <= 0) return false;
                           return (now.get() - l.activated_at.get()) >= l.duration_ns;
                       }),
        locks_.end());
}

bool LockRegistry::is_locked(LockType type, const std::string& target) const {
    return std::any_of(locks_.begin(), locks_.end(),
                       [&](const LockRecord& l) { return l.type == type && l.target == target; });
}

bool LockRegistry::has_symbol_lock(const std::string& symbol) const {
    return is_locked(LockType::SymbolLock, symbol);
}

bool LockRegistry::has_strategy_lock(const std::string& strategy_id) const {
    return is_locked(LockType::StrategyLock, strategy_id);
}

bool LockRegistry::has_day_lock() const {
    return std::any_of(locks_.begin(), locks_.end(),
                       [](const LockRecord& l) { return l.type == LockType::DayLock; });
}

bool LockRegistry::has_account_lock() const {
    return std::any_of(locks_.begin(), locks_.end(),
                       [](const LockRecord& l) { return l.type == LockType::AccountLock; });
}

bool LockRegistry::has_emergency_halt() const {
    return std::any_of(locks_.begin(), locks_.end(),
                       [](const LockRecord& l) { return l.type == LockType::EmergencyHalt; });
}

bool LockRegistry::has_cooldown(const std::string& target) const {
    return is_locked(LockType::Cooldown, target);
}

RiskStateLevel LockRegistry::compute_global_state() const {
    if (has_emergency_halt()) return RiskStateLevel::EmergencyHalt;
    if (has_account_lock()) return RiskStateLevel::AccountLock;
    if (has_day_lock()) return RiskStateLevel::DayLock;
    // Symbol/strategy locks don't affect global level to EmergencyHalt/AccountLock,
    // but they indicate Degraded state
    for (const auto& l : locks_) {
        if (l.type == LockType::StrategyLock) return RiskStateLevel::Degraded;
        if (l.type == LockType::SymbolLock) return RiskStateLevel::Degraded;
        if (l.type == LockType::Cooldown) return RiskStateLevel::Degraded;
    }
    return RiskStateLevel::Normal;
}

std::vector<LockRecord> LockRegistry::active_locks() const {
    return locks_;
}

int LockRegistry::count() const {
    return static_cast<int>(locks_.size());
}

void LockRegistry::clear_all() {
    locks_.clear();
}

// ═══════════════════════════════════════════════════════════════
// LossStreakTracker
// ═══════════════════════════════════════════════════════════════

void LossStreakTracker::record_trade_result(const std::string& symbol,
                                            const std::string& strategy_id,
                                            bool is_loss, Timestamp now) {
    if (is_loss) {
        ++total_consecutive_losses_;
        ++daily_stopouts_;
        last_loss_time_ = now;
        ++symbol_losses_[symbol];
        ++strategy_losses_[strategy_id];
    } else {
        // BUG-RS-06 fix: only reset the global consecutive-loss counter when the
        // symbol that just won was actually contributing to the current loss streak.
        // Without this check, a win on ANY symbol (e.g. ETHUSDT) resets the counter
        // even if the streak came entirely from a different symbol (e.g. BTCUSDT),
        // making the halt_after_n_losses / cooldown_after_n_losses triggers unreachable
        // during diversified trading.
        const int symbol_streak = symbol_losses_.count(symbol) ? symbol_losses_[symbol] : 0;
        if (symbol_streak > 0) {
            // This symbol had losses that contributed to the global streak — reset.
            total_consecutive_losses_ = 0;
        }
        // Always reset per-symbol and per-strategy counters for the winning symbol/strategy.
        symbol_losses_[symbol] = 0;
        strategy_losses_[strategy_id] = 0;
    }
}

void LossStreakTracker::reset_daily() {
    total_consecutive_losses_ = 0;
    daily_stopouts_ = 0;
    symbol_losses_.clear();
    strategy_losses_.clear();
}

int LossStreakTracker::symbol_consecutive_losses(const std::string& symbol) const {
    auto it = symbol_losses_.find(symbol);
    return it != symbol_losses_.end() ? it->second : 0;
}

int LossStreakTracker::strategy_consecutive_losses(const std::string& strategy_id) const {
    auto it = strategy_losses_.find(strategy_id);
    return it != strategy_losses_.end() ? it->second : 0;
}

// ═══════════════════════════════════════════════════════════════
// PnlTracker
// ═══════════════════════════════════════════════════════════════

void PnlTracker::record_trade_pnl(const std::string& symbol, const std::string& strategy_id,
                                   double realized_pnl) {
    // Account-level daily PnL and trade count are tracked by Portfolio (single source of truth).
    // Risk PnlTracker only tracks per-symbol and per-strategy breakdowns.
    symbol_pnl_[symbol] += realized_pnl;
    strategy_pnl_[strategy_id] += realized_pnl;
}

void PnlTracker::reset_daily() {
    symbol_pnl_.clear();
    strategy_pnl_.clear();
}

double PnlTracker::symbol_daily_pnl(const std::string& symbol) const {
    auto it = symbol_pnl_.find(symbol);
    return it != symbol_pnl_.end() ? it->second : 0.0;
}

double PnlTracker::strategy_daily_pnl(const std::string& strategy_id) const {
    auto it = strategy_pnl_.find(strategy_id);
    return it != strategy_pnl_.end() ? it->second : 0.0;
}

// ═══════════════════════════════════════════════════════════════
// DrawdownTracker
// ═══════════════════════════════════════════════════════════════

void DrawdownTracker::update_equity(double equity, Timestamp /*now*/) {
    current_equity_ = equity;
    if (equity > peak_equity_) peak_equity_ = equity;
    if (equity > intraday_peak_) intraday_peak_ = equity;
}

void DrawdownTracker::reset_intraday() {
    intraday_peak_ = current_equity_;
    // BUG-RS-10: reset peak_equity_ daily so account_drawdown_pct() reflects
    // today's drawdown, not all-time drawdown from session start months ago.
    peak_equity_ = current_equity_;
}

double DrawdownTracker::account_drawdown_pct() const {
    if (peak_equity_ <= 0.0) return 0.0;
    return std::max(0.0, (peak_equity_ - current_equity_) / peak_equity_ * 100.0);
}

double DrawdownTracker::intraday_drawdown_pct() const {
    if (intraday_peak_ <= 0.0) return 0.0;
    return std::max(0.0, (intraday_peak_ - current_equity_) / intraday_peak_ * 100.0);
}

// ═══════════════════════════════════════════════════════════════
// RateTracker
// ═══════════════════════════════════════════════════════════════

void RateTracker::purge_old(std::deque<int64_t>& q, int64_t cutoff) {
    while (!q.empty() && q.front() < cutoff) q.pop_front();
}

void RateTracker::record_order(Timestamp now) {
    order_timestamps_.push_back(now.get());
    purge_old(order_timestamps_, now.get() - 60'000'000'000LL);
}

void RateTracker::record_trade_close(Timestamp now) {
    trade_close_timestamps_.push_back(now.get());
    purge_old(trade_close_timestamps_, now.get() - 3'600'000'000'000LL);
}

void RateTracker::record_symbol_trade(const std::string& symbol, Timestamp now) {
    last_trade_per_symbol_[symbol] = now.get();
}

int RateTracker::orders_last_minute(Timestamp now) {
    purge_old(order_timestamps_, now.get() - 60'000'000'000LL);
    return static_cast<int>(order_timestamps_.size());
}

int RateTracker::trades_last_hour(Timestamp now) {
    purge_old(trade_close_timestamps_, now.get() - 3'600'000'000'000LL);
    return static_cast<int>(trade_close_timestamps_.size());
}

int64_t RateTracker::last_trade_for_symbol(const std::string& symbol) const {
    auto it = last_trade_per_symbol_.find(symbol);
    return it != last_trade_per_symbol_.end() ? it->second : 0;
}

// ═══════════════════════════════════════════════════════════════
// RiskState
// ═══════════════════════════════════════════════════════════════

void RiskState::reset_daily(Timestamp now) {
    loss_streaks.reset_daily();
    pnl.reset_daily();
    drawdown.reset_intraday();
    locks.remove_lock(LockType::DayLock, "");
    locks.clear_expired(now);

    for (auto& [key, budget] : strategy_budgets) {
        budget.daily_loss = 0.0;
        budget.daily_loss_pct = 0.0;
        budget.trades_today = 0;
        budget.consecutive_losses = 0;
    }
}

RiskStateLevel RiskState::global_level() const {
    return locks.compute_global_state();
}

} // namespace tb::risk
