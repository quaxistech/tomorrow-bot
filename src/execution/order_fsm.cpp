#include "execution/order_fsm.hpp"

namespace tb::execution {

OrderFSM::OrderFSM(OrderId order_id)
    : order_id_(std::move(order_id))
    , created_at_ms_(0)
    , last_transition_ms_(0)
{
}

OrderState OrderFSM::current_state() const {
    return state_;
}

bool OrderFSM::transition(OrderState new_state, const std::string& reason,
                           Timestamp now) {
    if (!is_valid_transition(state_, new_state)) {
        return false;
    }

    int64_t now_ms = now.get() / 1'000'000; // нс → мс (0 если не передано)

    history_.push_back(OrderTransition{
        .from = state_,
        .to = new_state,
        .reason = reason,
        .occurred_at = now
    });

    state_ = new_state;
    last_transition_ms_ = now_ms;
    return true;
}

const std::vector<OrderTransition>& OrderFSM::history() const {
    return history_;
}

bool OrderFSM::is_terminal() const {
    return state_ == OrderState::Filled || state_ == OrderState::Cancelled ||
           state_ == OrderState::Rejected || state_ == OrderState::Expired;
}

bool OrderFSM::is_active() const {
    return state_ == OrderState::Open || state_ == OrderState::PartiallyFilled ||
           state_ == OrderState::PendingAck || state_ == OrderState::CancelPending;
}

void OrderFSM::force_transition(OrderState new_state, const std::string& reason,
                                 Timestamp now) {
    int64_t now_ms = now.get() / 1'000'000;

    history_.push_back(OrderTransition{
        .from = state_,
        .to = new_state,
        .reason = "[FORCED] " + reason,
        .occurred_at = now
    });

    state_ = new_state;
    last_transition_ms_ = now_ms;
}

Timestamp OrderFSM::last_transition_time() const {
    if (history_.empty()) {
        return Timestamp(created_at_ms_);
    }
    return history_.back().occurred_at;
}

Timestamp OrderFSM::created_at() const {
    return Timestamp(created_at_ms_);
}

int64_t OrderFSM::time_in_current_state_ms(int64_t now_ms) const {
    return now_ms - last_transition_ms_;
}

} // namespace tb::execution
