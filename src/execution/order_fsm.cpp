#include "execution/order_fsm.hpp"

namespace tb::execution {

OrderFSM::OrderFSM(OrderId order_id)
    : order_id_(std::move(order_id))
{
}

OrderState OrderFSM::current_state() const {
    return state_;
}

bool OrderFSM::transition(OrderState new_state, const std::string& reason) {
    // Проверить допустимость перехода
    if (!is_valid_transition(state_, new_state)) {
        return false;
    }

    // Сохранить переход в истории
    history_.push_back(OrderTransition{
        .from = state_,
        .to = new_state,
        .reason = reason,
        .occurred_at = Timestamp(0) // Время устанавливается вызывающим кодом
    });

    state_ = new_state;
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

} // namespace tb::execution
