#include "execution/order_fsm.hpp"

namespace tb::execution {

// ---------- static helper ----------

Timestamp OrderFSM::wall_clock_now() noexcept {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Timestamp{ns};
}

// ---------- ctor ----------

OrderFSM::OrderFSM(OrderId order_id)
    : order_id_(std::move(order_id))
    , created_at_ns_(wall_clock_now().get())
    , last_transition_tp_(std::chrono::steady_clock::now())
{
}

// ---------- state queries ----------

OrderState OrderFSM::current_state() const {
    return state_;
}

// ---------- transitions ----------

bool OrderFSM::transition(OrderState new_state, const std::string& reason,
                           Timestamp now) {
    if (!is_valid_transition(state_, new_state)) {
        return false;
    }

    // Если вызывающая сторона не передала метку времени — подставить wall-clock
    if (now == Timestamp(0)) {
        now = wall_clock_now();
    }

    history_.push_back(OrderTransition{
        .from = state_,
        .to = new_state,
        .reason = reason,
        .occurred_at = now
    });

    state_ = new_state;
    last_transition_tp_ = std::chrono::steady_clock::now();
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
    // Если вызывающая сторона не передала метку времени — подставить wall-clock
    if (now == Timestamp(0)) {
        now = wall_clock_now();
    }

    history_.push_back(OrderTransition{
        .from = state_,
        .to = new_state,
        .reason = "[FORCED] " + reason,
        .occurred_at = now
    });

    state_ = new_state;
    last_transition_tp_ = std::chrono::steady_clock::now();
}

// ---------- time accessors ----------

Timestamp OrderFSM::last_transition_time() const {
    if (history_.empty()) {
        return Timestamp(created_at_ns_);
    }
    return history_.back().occurred_at;
}

Timestamp OrderFSM::created_at() const {
    return Timestamp(created_at_ns_);
}

int64_t OrderFSM::time_in_current_state_ms() const {
    auto elapsed = std::chrono::steady_clock::now() - last_transition_tp_;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

} // namespace tb::execution
