#include "pipeline/periodic_trailing_sl.hpp"

#include <algorithm>
#include <cmath>

namespace tb::pipeline {

PeriodicTrailingSl::PeriodicTrailingSl(
    std::shared_ptr<ProtectiveBracketManager> bracket_manager,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    PeriodicTrailingConfig cfg)
    : bracket_manager_(std::move(bracket_manager))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , cfg_(cfg) {}

bool PeriodicTrailingSl::is_monotonic_improvement(double current_sl,
                                                    double new_sl,
                                                    PositionSide ps) const {
    if (current_sl <= 0.0 || new_sl <= 0.0) return false;
    if (ps == PositionSide::Long) {
        return new_sl > current_sl;  // SL вверх = ближе к BE = меньше риск
    }
    return new_sl < current_sl;  // SHORT: SL вниз = ближе к BE
}

double PeriodicTrailingSl::compute_new_sl(const TrailingPositionSnapshot& snap) const {
    if (snap.atr <= 0.0 || !std::isfinite(snap.atr)) return 0.0;
    if (snap.entry_price <= 0.0) return 0.0;
    if (snap.current_price <= 0.0) return 0.0;

    // run94: adaptive ATR multiplier based on contextual indicators.
    // Default multiplier — base. Корректируется по сигналам:
    //   - Supertrend против позиции → tighten ×0.6 (быстрый exit)
    //   - CVD divergence против позиции → tighten ×0.7
    //   - High cascade risk → tighten ×0.5
    double trail_mult = cfg_.atr_trail_multiplier;
    bool trend_against = (snap.position_side == PositionSide::Long && snap.supertrend_trend == -1)
                       || (snap.position_side == PositionSide::Short && snap.supertrend_trend == 1);
    if (trend_against) trail_mult *= 0.6;

    bool div_against = (snap.position_side == PositionSide::Long && snap.cvd_bearish_div)
                     || (snap.position_side == PositionSide::Short && snap.cvd_bullish_div);
    if (div_against) trail_mult *= 0.7;

    if (snap.liq_cascade_risk > 0.7) trail_mult *= 0.5;

    // Bug 3.1 fix: clamp trail_mult минимум 0.4 ATR. Если все 3 триггера сработают
    // одновременно (0.9 × 0.6 × 0.7 × 0.5 = 0.189) → SL слишком близко → noise trigger.
    // 0.4 ATR гарантирует разумный safety margin даже в worst case.
    trail_mult = std::max(trail_mult, 0.4);

    double raw_sl = 0.0;
    if (snap.position_side == PositionSide::Long) {
        double high = std::max(snap.highest_since_entry, snap.current_price);
        if (high <= 0.0) return 0.0;
        raw_sl = high - snap.atr * trail_mult;
    } else {
        double low = (snap.lowest_since_entry > 0.0)
            ? std::min(snap.lowest_since_entry, snap.current_price)
            : snap.current_price;
        if (low <= 0.0) return 0.0;
        raw_sl = low + snap.atr * trail_mult;
    }

    // B4.2 fix: fee constants теперь из config (поддерживает VIP-уровни Bitget).
    // Раньше захардкожено 12 + 3 = 15 bps. Теперь — cfg_.round_trip_fee_bps.
    const double breakeven_buffer_pct = (cfg_.round_trip_fee_bps + cfg_.safety_bps) / 10000.0;

    if (snap.position_side == PositionSide::Long) {
        // For LONG: SL должен быть ≥ entry × (1 + buffer) чтобы close at SL = profit
        double min_safe_sl = snap.entry_price * (1.0 + breakeven_buffer_pct);
        return std::max(raw_sl, min_safe_sl);
    } else {
        // For SHORT: SL должен быть ≤ entry × (1 - buffer)
        double min_safe_sl = snap.entry_price * (1.0 - breakeven_buffer_pct);
        return std::min(raw_sl, min_safe_sl);
    }
}

int PeriodicTrailingSl::tick(const TrailingPositionSnapshot& snap) {
    if (!cfg_.enabled || !bracket_manager_) return 0;

    const int64_t now_ns = clock_ ? clock_->now().get() : 0;
    const int64_t prev_ns = last_tick_ns_.load(std::memory_order_acquire);
    if (prev_ns > 0 && (now_ns - prev_ns) < cfg_.min_interval_ms * 1'000'000) {
        return 0;
    }
    last_tick_ns_.store(now_ns, std::memory_order_release);

    // 1. Bracket state — нужен текущий SL.
    auto state_opt = bracket_manager_->get_state(snap.symbol, snap.position_side);
    if (!state_opt.has_value()) return 0;
    const auto& state = state_opt.value();
    if (state.released) return 0;
    if (state.sl_price <= 0.0) return 0;

    // 2. Profitable activation guard — не дёргаем SL пока сделка в minus.
    //    Иначе при volatile noise trail срезает позицию на breakeven слишком
    //    рано и не даёт алгоритму шанс развернуться.
    double profit_bps = 0.0;
    if (snap.entry_price > 0.0) {
        if (snap.position_side == PositionSide::Long) {
            profit_bps = (snap.current_price - snap.entry_price) / snap.entry_price * 10000.0;
        } else {
            profit_bps = (snap.entry_price - snap.current_price) / snap.entry_price * 10000.0;
        }
    }
    if (profit_bps < cfg_.activation_min_profit_bps) {
        updates_skipped_not_profitable_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // 3. Считаем новый SL по Chandelier.
    double new_sl = compute_new_sl(snap);
    if (new_sl <= 0.0) return 0;

    // 4. Monotonic check — двигаем только в сторону прибыли.
    if (!is_monotonic_improvement(state.sl_price, new_sl, snap.position_side)) {
        return 0;
    }

    // 5. Min-move guard — не дёргаем биржу на копеечные изменения.
    double move_bps = std::abs(new_sl - state.sl_price) / state.sl_price * 10000.0;
    if (move_bps < cfg_.min_sl_move_bps) {
        updates_skipped_small_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // 6. Apply через ProtectiveBracketManager — он cancel'нет старый plan
    //    и поставит новый. Если fail — текущий SL остаётся.
    bool ok = bracket_manager_->update_sl(snap.symbol, snap.position_side, new_sl);
    if (ok) {
        updates_applied_.fetch_add(1, std::memory_order_relaxed);
        if (logger_) {
            logger_->info("trailing_sl",
                "SL подтянут (monotonic)",
                {{"symbol", snap.symbol.get()},
                 {"position_side", std::string(tb::to_string(snap.position_side))},
                 {"old_sl", std::to_string(state.sl_price)},
                 {"new_sl", std::to_string(new_sl)},
                 {"move_bps", std::to_string(move_bps)},
                 {"profit_bps", std::to_string(profit_bps)},
                 {"current_price", std::to_string(snap.current_price)}});
        }
        return 1;
    }
    return 0;
}

} // namespace tb::pipeline
