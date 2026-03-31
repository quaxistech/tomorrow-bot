#include "execution/order_types.hpp"

namespace tb::execution {

std::string to_string(OrderState state) {
    switch (state) {
        case OrderState::New:              return "New";
        case OrderState::PendingAck:       return "PendingAck";
        case OrderState::Open:             return "Open";
        case OrderState::PartiallyFilled:  return "PartiallyFilled";
        case OrderState::Filled:           return "Filled";
        case OrderState::CancelPending:    return "CancelPending";
        case OrderState::Cancelled:        return "Cancelled";
        case OrderState::Rejected:         return "Rejected";
        case OrderState::Expired:          return "Expired";
        case OrderState::UnknownRecovery:  return "UnknownRecovery";
    }
    return "Unknown";
}

std::string to_string(PartialFillPolicy policy) {
    switch (policy) {
        case PartialFillPolicy::WaitForFull:      return "WaitForFull";
        case PartialFillPolicy::CancelRemaining:  return "CancelRemaining";
        case PartialFillPolicy::AllowPartial:     return "AllowPartial";
    }
    return "Unknown";
}

bool is_valid_transition(OrderState from, OrderState to) {
    // Терминальные состояния — переходы из них недопустимы
    if (from == OrderState::Filled || from == OrderState::Cancelled ||
        from == OrderState::Rejected || from == OrderState::Expired) {
        return false;
    }

    // Разрешённые переходы
    switch (from) {
        case OrderState::New:
            return to == OrderState::PendingAck;

        case OrderState::PendingAck:
            return to == OrderState::Open ||
                   to == OrderState::Rejected ||
                   to == OrderState::Filled ||
                   to == OrderState::PartiallyFilled ||
                   to == OrderState::CancelPending ||
                   to == OrderState::Cancelled ||
                   to == OrderState::UnknownRecovery;

        case OrderState::Open:
            return to == OrderState::PartiallyFilled ||
                   to == OrderState::Filled ||
                   to == OrderState::CancelPending ||
                   to == OrderState::Cancelled ||
                   to == OrderState::Expired ||
                   to == OrderState::UnknownRecovery;

        case OrderState::PartiallyFilled:
            return to == OrderState::Filled ||
                   to == OrderState::CancelPending ||
                   to == OrderState::Cancelled ||
                   to == OrderState::Expired ||
                   to == OrderState::UnknownRecovery;

        case OrderState::CancelPending:
            return to == OrderState::Cancelled ||
                   to == OrderState::Filled ||
                   to == OrderState::PartiallyFilled ||
                   to == OrderState::UnknownRecovery;

        case OrderState::UnknownRecovery:
            // Из восстановления можно перейти в любое состояние
            return true;

        default:
            return false;
    }
}

} // namespace tb::execution
