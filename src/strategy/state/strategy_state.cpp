#include "strategy/state/strategy_state.hpp"
#include <algorithm>
#include <chrono>

namespace tb::strategy {

StrategyStateMachine::StrategyStateMachine(const ScalpStrategyConfig& cfg)
    : cfg_(cfg) {}

bool StrategyStateMachine::transition_to(SymbolState new_state, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    // Валидация допустимых переходов
    bool valid = false;
    switch (state_) {
        case SymbolState::Idle:
            valid = (new_state == SymbolState::Candidate ||
                     new_state == SymbolState::SetupForming ||  // прямой переход при мгновенном обнаружении
                     new_state == SymbolState::PositionOpen ||  // восстановление
                     new_state == SymbolState::Cooldown ||      // recovery: position closed while FSM was Idle
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::Candidate:
            valid = (new_state == SymbolState::SetupForming ||
                     new_state == SymbolState::PositionOpen ||  // восстановление после fill/recovery
                     new_state == SymbolState::Idle ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::SetupForming:
            valid = (new_state == SymbolState::SetupPendingConfirmation ||
                     new_state == SymbolState::PositionOpen ||  // позиция появилась до завершения setup lifecycle
                     new_state == SymbolState::Idle ||  // отмена
                     new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::SetupPendingConfirmation:
            valid = (new_state == SymbolState::EntryReady ||
                     new_state == SymbolState::PositionOpen ||  // recovery/sync после fill
                     new_state == SymbolState::Idle ||  // отмена
                     new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::EntryReady:
            valid = (new_state == SymbolState::EntrySent ||
                     new_state == SymbolState::PositionOpen ||  // fill может быть подтверждён раньше feedback
                     new_state == SymbolState::Idle ||
                     new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::EntrySent:
            valid = (new_state == SymbolState::PositionOpen ||
                     new_state == SymbolState::Idle ||
                     new_state == SymbolState::Cooldown);
            break;
        case SymbolState::PositionOpen:
            valid = (new_state == SymbolState::PositionManaging ||
                     new_state == SymbolState::ExitPending ||
                     new_state == SymbolState::Cooldown);
            break;
        case SymbolState::PositionManaging:
            valid = (new_state == SymbolState::ExitPending ||
                     new_state == SymbolState::PositionOpen ||
                     new_state == SymbolState::Cooldown);
            break;
        case SymbolState::ExitPending:
            valid = (new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Idle);
            break;
        case SymbolState::Cooldown:
            valid = (new_state == SymbolState::Idle);
            break;
        case SymbolState::Blocked:
            valid = (new_state == SymbolState::Idle);
            break;
    }

    if (valid) {
        state_ = new_state;
        last_transition_ns_ = now_ns;
    }
    return valid;
}

void StrategyStateMachine::set_setup(Setup setup) {
    std::lock_guard lock(mutex_);
    active_setup_ = std::move(setup);
}

void StrategyStateMachine::clear_setup() {
    std::lock_guard lock(mutex_);
    active_setup_.reset();
}

void StrategyStateMachine::start_cooldown(int64_t now_ns, int64_t duration_ms) {
    std::lock_guard lock(mutex_);
    if (now_ns <= 0) return; // Bad clock — don't set a cooldown with invalid time
    // BUG-RS-14 fix: use std::chrono to avoid int64_t overflow when duration_ms
    // exceeds ~9 million ms (~150 min). Manual multiplication (duration_ms * 1'000'000LL)
    // overflows for large values; chrono arithmetic is safe and expressive.
    using namespace std::chrono;
    const int64_t duration_ns =
        duration_cast<nanoseconds>(milliseconds(duration_ms)).count();
    cooldown_end_ns_ = now_ns + duration_ns;

    // BUG-S31-01 fix: always go through transition_to() — direct state_ assignment
    // bypasses FSM invariants that other components rely on. If the current state
    // doesn't allow Cooldown the transition is silently dropped; callers that need
    // forced cooldown should first ensure the FSM is in a compatible state.
    transition_to(SymbolState::Cooldown, now_ns);
}

bool StrategyStateMachine::is_cooldown_expired(int64_t now_ns) const {
    std::lock_guard lock(mutex_);
    if (now_ns <= 0) return false; // Bad clock — conservatively keep cooldown active
    return now_ns >= cooldown_end_ns_;
}

void StrategyStateMachine::open_position(const Setup& setup, double entry_price, double size,
                                         Side side, PositionSide pos_side, double atr, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    position_ctx_ = StrategyPositionContext{};
    position_ctx_.has_position = true;
    position_ctx_.side = side;
    position_ctx_.position_side = pos_side;
    position_ctx_.size = size;
    position_ctx_.avg_entry_price = entry_price;
    position_ctx_.entry_time_ns = now_ns;
    position_ctx_.entry_setup_id = setup.id;
    position_ctx_.entry_setup_type = setup.type;
    position_ctx_.entry_confidence = setup.confidence;
    position_ctx_.entry_atr = atr;
    position_ctx_.peak_favorable_price = entry_price;
}

void StrategyStateMachine::update_position(double current_price, double unrealized_pnl, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    if (!position_ctx_.has_position) return;

    position_ctx_.unrealized_pnl = unrealized_pnl;
    position_ctx_.hold_duration_ns = now_ns - position_ctx_.entry_time_ns;

    if (position_ctx_.avg_entry_price > 0.0) {
        position_ctx_.unrealized_pnl_pct =
            (current_price - position_ctx_.avg_entry_price) / position_ctx_.avg_entry_price;
        if (position_ctx_.position_side == PositionSide::Short) {
            position_ctx_.unrealized_pnl_pct = -position_ctx_.unrealized_pnl_pct;
        }
    }

    position_ctx_.peak_pnl_pct = std::max(position_ctx_.peak_pnl_pct, position_ctx_.unrealized_pnl_pct);
    if (position_ctx_.position_side == PositionSide::Long) {
        position_ctx_.peak_favorable_price = std::max(position_ctx_.peak_favorable_price, current_price);
    } else {
        position_ctx_.peak_favorable_price = std::min(position_ctx_.peak_favorable_price, current_price);
    }
}

void StrategyStateMachine::close_position() {
    std::lock_guard lock(mutex_);
    position_ctx_ = StrategyPositionContext{};
}

void StrategyStateMachine::block(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    state_ = SymbolState::Blocked;
    last_transition_ns_ = now_ns;
}

void StrategyStateMachine::unblock(int64_t now_ns) {
    std::lock_guard lock(mutex_);
    if (state_ == SymbolState::Blocked) {
        state_ = SymbolState::Idle;
        last_transition_ns_ = now_ns;
    }
}

void StrategyStateMachine::reset() {
    std::lock_guard lock(mutex_);
    // BUG-S31-02: preserve position_ctx_ if a position is active so the pipeline
    // can still observe and close it. Callers must call close_position() explicitly
    // before reset() when a position is open to avoid orphaned positions.
    if (!position_ctx_.has_position) {
        position_ctx_ = StrategyPositionContext{};
    }
    state_ = SymbolState::Idle;
    active_setup_.reset();
    last_transition_ns_ = 0;
    cooldown_end_ns_ = 0;
    setup_counter_ = 0;
}

std::string StrategyStateMachine::next_setup_id() {
    std::lock_guard lock(mutex_);
    return "setup_" + std::to_string(++setup_counter_);
}

} // namespace tb::strategy
