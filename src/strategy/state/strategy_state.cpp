#include "strategy/state/strategy_state.hpp"
#include <algorithm>

namespace tb::strategy {

StrategyStateMachine::StrategyStateMachine(const ScalpStrategyConfig& cfg)
    : cfg_(cfg) {}

bool StrategyStateMachine::transition_to(SymbolState new_state, int64_t now_ns) {
    // Валидация допустимых переходов
    bool valid = false;
    switch (state_) {
        case SymbolState::Idle:
            valid = (new_state == SymbolState::Candidate ||
                     new_state == SymbolState::SetupForming ||  // прямой переход при мгновенном обнаружении
                     new_state == SymbolState::PositionOpen ||  // восстановление
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::Candidate:
            valid = (new_state == SymbolState::SetupForming ||
                     new_state == SymbolState::Idle ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::SetupForming:
            valid = (new_state == SymbolState::SetupPendingConfirmation ||
                     new_state == SymbolState::Idle ||  // отмена
                     new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::SetupPendingConfirmation:
            valid = (new_state == SymbolState::EntryReady ||
                     new_state == SymbolState::Idle ||  // отмена
                     new_state == SymbolState::Cooldown ||
                     new_state == SymbolState::Blocked);
            break;
        case SymbolState::EntryReady:
            valid = (new_state == SymbolState::EntrySent ||
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
    active_setup_ = std::move(setup);
}

void StrategyStateMachine::clear_setup() {
    active_setup_.reset();
}

void StrategyStateMachine::start_cooldown(int64_t now_ns, int64_t duration_ms) {
    cooldown_end_ns_ = now_ns + duration_ms * 1'000'000LL;
    // Прямое присвоение — cooldown может начаться из любого состояния
    state_ = SymbolState::Cooldown;
    last_transition_ns_ = now_ns;
}

bool StrategyStateMachine::is_cooldown_expired(int64_t now_ns) const {
    return now_ns >= cooldown_end_ns_;
}

void StrategyStateMachine::open_position(const Setup& setup, double entry_price, double size,
                                         Side side, PositionSide pos_side, double atr, int64_t now_ns) {
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
    position_ctx_ = StrategyPositionContext{};
}

void StrategyStateMachine::block(int64_t now_ns) {
    state_ = SymbolState::Blocked;
    last_transition_ns_ = now_ns;
}

void StrategyStateMachine::unblock(int64_t now_ns) {
    if (state_ == SymbolState::Blocked) {
        state_ = SymbolState::Idle;
        last_transition_ns_ = now_ns;
    }
}

void StrategyStateMachine::reset() {
    state_ = SymbolState::Idle;
    active_setup_.reset();
    position_ctx_ = StrategyPositionContext{};
    last_transition_ns_ = 0;
    cooldown_end_ns_ = 0;
    setup_counter_ = 0;
}

std::string StrategyStateMachine::next_setup_id() {
    return "setup_" + std::to_string(++setup_counter_);
}

} // namespace tb::strategy
