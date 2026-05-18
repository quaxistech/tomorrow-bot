#include "telemetry/trade_journal.hpp"

#include <algorithm>
#include <cmath>

namespace tb::telemetry {

std::string TradeJournal::make_key(const Symbol& s, PositionSide ps) {
    return s.get() + ":" + std::string(tb::to_string(ps));
}

void TradeJournal::on_entry_filled(const Symbol& symbol,
                                    PositionSide position_side,
                                    double entry_price,
                                    double position_size,
                                    int64_t signal_snapshot_ts_ns,
                                    const std::string& strategy_id,
                                    const std::string& setup_type,
                                    double conviction) {
    InFlightTrade t;
    t.row.symbol = symbol;
    t.row.position_side = position_side;
    t.row.signal_snapshot_ts_ns = signal_snapshot_ts_ns;
    t.row.entry_filled_ts_ns = clock_ ? clock_->now().get() : 0;
    t.row.entry_price = entry_price;
    t.row.position_size = position_size;
    t.row.strategy_id = strategy_id;
    t.row.setup_type = setup_type;
    t.row.conviction = conviction;

    if (signal_snapshot_ts_ns > 0 && t.row.entry_filled_ts_ns > 0) {
        t.row.signal_age_ms = (t.row.entry_filled_ts_ns - signal_snapshot_ts_ns) / 1'000'000;
    }

    t.max_favorable_price = entry_price;
    t.max_adverse_price = entry_price;
    t.last_tick_ns = t.row.entry_filled_ts_ns;

    const std::string key = make_key(symbol, position_side);
    {
        std::lock_guard lock(mutex_);
        in_flight_[key] = t;
    }

    if (logger_) {
        logger_->info("trade_journal",
            "Entry registered",
            {{"symbol", symbol.get()},
             {"position_side", std::string(tb::to_string(position_side))},
             {"entry_price", std::to_string(entry_price)},
             {"size", std::to_string(position_size)},
             {"signal_age_ms", std::to_string(t.row.signal_age_ms)},
             {"strategy_id", strategy_id},
             {"setup_type", setup_type}});
    }
}

void TradeJournal::on_tick(const Symbol& symbol, PositionSide position_side, double current_price) {
    if (!(current_price > 0.0) || !std::isfinite(current_price)) return;

    const std::string key = make_key(symbol, position_side);
    std::lock_guard lock(mutex_);
    auto it = in_flight_.find(key);
    if (it == in_flight_.end()) return;
    auto& t = it->second;

    const bool is_long = (position_side == PositionSide::Long);
    if (is_long) {
        if (current_price > t.max_favorable_price) t.max_favorable_price = current_price;
        if (current_price < t.max_adverse_price)   t.max_adverse_price = current_price;
    } else {
        // SHORT: favorable = price DOWN, adverse = price UP
        if (current_price < t.max_favorable_price || t.max_favorable_price == 0.0) {
            t.max_favorable_price = current_price;
        }
        if (current_price > t.max_adverse_price) t.max_adverse_price = current_price;
    }
    t.last_tick_ns = clock_ ? clock_->now().get() : t.last_tick_ns;
}

void TradeJournal::on_exit_filled(const Symbol& symbol,
                                    PositionSide position_side,
                                    double exit_price,
                                    double gross_pnl,
                                    double fees_paid,
                                    ExitLayer exit_layer,
                                    const std::string& exit_reason) {
    const std::string key = make_key(symbol, position_side);

    InFlightTrade snapshot;
    bool found = false;
    {
        std::lock_guard lock(mutex_);
        auto it = in_flight_.find(key);
        if (it != in_flight_.end()) {
            snapshot = it->second;
            in_flight_.erase(it);
            found = true;
        }
    }

    if (!found) {
        if (logger_) {
            logger_->warn("trade_journal",
                "Exit без зарегистрированного entry — пропуск journaling",
                {{"symbol", symbol.get()},
                 {"position_side", std::string(tb::to_string(position_side))}});
        }
        return;
    }

    TradeRow& row = snapshot.row;
    row.exit_filled_ts_ns = clock_ ? clock_->now().get() : 0;
    row.exit_price = exit_price;
    row.gross_pnl = gross_pnl;
    row.fees_paid = fees_paid;
    row.net_pnl = gross_pnl - fees_paid;
    row.exit_layer = exit_layer;
    row.exit_reason = exit_reason;

    if (row.entry_filled_ts_ns > 0 && row.exit_filled_ts_ns > row.entry_filled_ts_ns) {
        row.hold_duration_ms = (row.exit_filled_ts_ns - row.entry_filled_ts_ns) / 1'000'000;
    }

    // Compute MFE / MAE / giveback in bps от entry_price.
    const bool is_long = (position_side == PositionSide::Long);
    if (row.entry_price > 0.0) {
        if (is_long) {
            row.mfe_bps = (snapshot.max_favorable_price - row.entry_price) / row.entry_price * 10000.0;
            row.mae_bps = (row.entry_price - snapshot.max_adverse_price) / row.entry_price * 10000.0;
        } else {
            row.mfe_bps = (row.entry_price - snapshot.max_favorable_price) / row.entry_price * 10000.0;
            row.mae_bps = (snapshot.max_adverse_price - row.entry_price) / row.entry_price * 10000.0;
        }
        if (row.mfe_bps < 0.0) row.mfe_bps = 0.0;
        if (row.mae_bps < 0.0) row.mae_bps = 0.0;
    }

    // net_pnl в bps от notional = (net_pnl / notional) × 10000.
    double net_pnl_bps = 0.0;
    if (row.entry_price > 0.0 && row.position_size > 0.0) {
        const double notional = row.entry_price * row.position_size;
        net_pnl_bps = (notional > 0.0) ? (row.net_pnl / notional * 10000.0) : 0.0;
    }
    row.giveback_bps = std::max(0.0, row.mfe_bps - net_pnl_bps);

    // B26.1 fix: estimate slippage как (giveback от mfe) — индикативно показывает,
    // насколько мы "оставили на столе" из-за поздних exit или slippage.
    row.slippage_bps = row.giveback_bps;

    {
        std::lock_guard lock(mutex_);
        // B26.2 fix: capped collection. Защита от unbounded memory growth.
        constexpr size_t kMaxJournalEntries = 5000;
        if (closed_.size() >= kMaxJournalEntries) {
            // Удаляем 10% старейших чтобы избежать частых cleanup'ов.
            closed_.erase(closed_.begin(),
                closed_.begin() + static_cast<std::ptrdiff_t>(kMaxJournalEntries / 10));
        }
        closed_.push_back(row);
    }

    if (logger_) {
        logger_->info("trade_journal",
            "Trade closed",
            {{"symbol", row.symbol.get()},
             {"position_side", std::string(tb::to_string(position_side))},
             {"signal_age_ms", std::to_string(row.signal_age_ms)},
             {"hold_ms", std::to_string(row.hold_duration_ms)},
             {"entry", std::to_string(row.entry_price)},
             {"exit", std::to_string(row.exit_price)},
             {"size", std::to_string(row.position_size)},
             {"gross_pnl", std::to_string(row.gross_pnl)},
             {"fees", std::to_string(row.fees_paid)},
             {"net_pnl", std::to_string(row.net_pnl)},
             {"mfe_bps", std::to_string(row.mfe_bps)},
             {"mae_bps", std::to_string(row.mae_bps)},
             {"giveback_bps", std::to_string(row.giveback_bps)},
             {"net_pnl_bps", std::to_string(net_pnl_bps)},
             {"exit_layer", to_string(exit_layer)},
             {"exit_reason", exit_reason},
             {"setup_type", row.setup_type},
             {"conviction", std::to_string(row.conviction)}});
    }
}

std::optional<InFlightTrade> TradeJournal::get_in_flight(const Symbol& symbol,
                                                          PositionSide ps) const {
    std::lock_guard lock(mutex_);
    auto it = in_flight_.find(make_key(symbol, ps));
    if (it == in_flight_.end()) return std::nullopt;
    return it->second;
}

std::vector<TradeRow> TradeJournal::closed_rows() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

void TradeJournal::clear_closed() {
    std::lock_guard lock(mutex_);
    closed_.clear();
}

} // namespace tb::telemetry
