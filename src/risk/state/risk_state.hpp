#pragma once
#include "risk/risk_types.hpp"
#include "common/types.hpp"
#include <mutex>
#include <unordered_map>
#include <deque>
#include <vector>
#include <string>

namespace tb::risk {

// ═══════════════════════════════════════════════════════════════
// Lock Registry — управление блокировками
// ═══════════════════════════════════════════════════════════════

class LockRegistry {
public:
    void add_lock(LockType type, const std::string& target,
                  const std::string& reason, Timestamp now, int64_t duration_ns = 0);
    void remove_lock(LockType type, const std::string& target);
    void clear_expired(Timestamp now);

    [[nodiscard]] bool is_locked(LockType type, const std::string& target) const;
    [[nodiscard]] bool has_symbol_lock(const std::string& symbol) const;
    [[nodiscard]] bool has_strategy_lock(const std::string& strategy_id) const;
    [[nodiscard]] bool has_day_lock() const;
    [[nodiscard]] bool has_account_lock() const;
    [[nodiscard]] bool has_emergency_halt() const;
    [[nodiscard]] bool has_cooldown(const std::string& target) const;

    [[nodiscard]] RiskStateLevel compute_global_state() const;
    [[nodiscard]] std::vector<LockRecord> active_locks() const;
    [[nodiscard]] int count() const;

    void clear_all();

private:
    std::vector<LockRecord> locks_;
};

// ═══════════════════════════════════════════════════════════════
// Loss Streak Tracker — отслеживание серий убытков
// ═══════════════════════════════════════════════════════════════

class LossStreakTracker {
public:
    void record_trade_result(const std::string& symbol, const std::string& strategy_id,
                             bool is_loss, Timestamp now);
    void reset_daily();

    [[nodiscard]] int total_consecutive_losses() const { return total_consecutive_losses_; }
    [[nodiscard]] int symbol_consecutive_losses(const std::string& symbol) const;
    [[nodiscard]] int strategy_consecutive_losses(const std::string& strategy_id) const;
    [[nodiscard]] int daily_stopouts() const { return daily_stopouts_; }
    [[nodiscard]] Timestamp last_loss_time() const { return last_loss_time_; }

private:
    int total_consecutive_losses_{0};
    int daily_stopouts_{0};
    Timestamp last_loss_time_{Timestamp(0)};
    std::unordered_map<std::string, int> symbol_losses_;
    std::unordered_map<std::string, int> strategy_losses_;
};

// ═══════════════════════════════════════════════════════════════
// PnL Tracker — отслеживание дневного PnL
// ═══════════════════════════════════════════════════════════════

class PnlTracker {
public:
    void record_trade_pnl(const std::string& symbol, const std::string& strategy_id,
                          double realized_pnl);
    void reset_daily();

    /// Per-symbol/strategy breakdowns (unique to risk — not duplicated in portfolio)
    [[nodiscard]] double symbol_daily_pnl(const std::string& symbol) const;
    [[nodiscard]] double strategy_daily_pnl(const std::string& strategy_id) const;

private:
    std::unordered_map<std::string, double> symbol_pnl_;
    std::unordered_map<std::string, double> strategy_pnl_;
};

// ═══════════════════════════════════════════════════════════════
// Drawdown Tracker — отслеживание просадки
// ═══════════════════════════════════════════════════════════════

class DrawdownTracker {
public:
    void update_equity(double equity, Timestamp now);
    void reset_intraday();

    [[nodiscard]] double peak_equity() const { return peak_equity_; }
    [[nodiscard]] double account_drawdown_pct() const;
    [[nodiscard]] double intraday_drawdown_pct() const;

private:
    double peak_equity_{0.0};
    double current_equity_{0.0};
    double intraday_peak_{0.0};
};

// ═══════════════════════════════════════════════════════════════
// Rate Tracker — ордера/сделки в единицу времени
// ═══════════════════════════════════════════════════════════════

class RateTracker {
public:
    void record_order(Timestamp now);
    void record_trade_close(Timestamp now);
    void record_symbol_trade(const std::string& symbol, Timestamp now);

    [[nodiscard]] int orders_last_minute(Timestamp now);
    [[nodiscard]] int trades_last_hour(Timestamp now);
    [[nodiscard]] int64_t last_trade_for_symbol(const std::string& symbol) const;

private:
    void purge_old(std::deque<int64_t>& q, int64_t cutoff);

    std::deque<int64_t> order_timestamps_;
    std::deque<int64_t> trade_close_timestamps_;
    std::unordered_map<std::string, int64_t> last_trade_per_symbol_;
};

// ═══════════════════════════════════════════════════════════════
// RiskState — агрегированное состояние риска
// ═══════════════════════════════════════════════════════════════

class RiskState {
public:
    LockRegistry locks;
    LossStreakTracker loss_streaks;
    PnlTracker pnl;
    DrawdownTracker drawdown;
    RateTracker rates;

    /// Бюджеты стратегий
    std::unordered_map<std::string, StrategyRiskBudget> strategy_budgets;

    void reset_daily(Timestamp now);
    [[nodiscard]] RiskStateLevel global_level() const;
};

} // namespace tb::risk
