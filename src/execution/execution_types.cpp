#include "execution/execution_types.hpp"

namespace tb::execution {

std::string ExecutionPlan::summary() const {
    std::string s;
    s += "action=" + to_string(action);
    s += " style=" + to_string(style);
    s += " type=";
    s += (order_type == OrderType::Market ? "market" : "limit");
    s += " qty=" + std::to_string(planned_quantity.get());
    if (planned_price.get() > 0.0) {
        s += " price=" + std::to_string(planned_price.get());
    }
    s += " timeout=" + std::to_string(timeout_ms) + "ms";
    if (enable_fallback) {
        s += " fallback@" + std::to_string(fallback_after_ms) + "ms";
    }
    if (reduce_only) s += " [reduce-only]";
    return s;
}

std::string to_string(ExecutionAction action) {
    switch (action) {
        case ExecutionAction::OpenPosition:           return "OpenPosition";
        case ExecutionAction::IncreasePosition:       return "IncreasePosition";
        case ExecutionAction::ReducePosition:         return "ReducePosition";
        case ExecutionAction::ClosePosition:          return "ClosePosition";
        case ExecutionAction::CancelOrder:            return "CancelOrder";
        case ExecutionAction::CancelAllForSymbol:     return "CancelAllForSymbol";
        case ExecutionAction::CancelAllGlobal:        return "CancelAllGlobal";
        case ExecutionAction::ReplaceOrder:           return "ReplaceOrder";
        case ExecutionAction::EmergencyFlattenSymbol: return "EmergencyFlattenSymbol";
        case ExecutionAction::EmergencyFlattenAll:    return "EmergencyFlattenAll";
    }
    return "Unknown";
}

std::string to_string(PlannedExecutionStyle style) {
    switch (style) {
        case PlannedExecutionStyle::AggressiveMarket:   return "AggressiveMarket";
        case PlannedExecutionStyle::PassiveLimit:       return "PassiveLimit";
        case PlannedExecutionStyle::PostOnlyLimit:      return "PostOnlyLimit";
        case PlannedExecutionStyle::SmartFallback:      return "SmartFallback";
        case PlannedExecutionStyle::CancelIfNotFilled:  return "CancelIfNotFilled";
        case PlannedExecutionStyle::ReduceOnly:         return "ReduceOnly";
    }
    return "Unknown";
}

std::string to_string(IntentState state) {
    switch (state) {
        case IntentState::Received:             return "Received";
        case IntentState::Validated:            return "Validated";
        case IntentState::Planned:              return "Planned";
        case IntentState::SubmissionStarted:    return "SubmissionStarted";
        case IntentState::Working:              return "Working";
        case IntentState::PartiallyExecuted:    return "PartiallyExecuted";
        case IntentState::Executed:             return "Executed";
        case IntentState::Cancelled:            return "Cancelled";
        case IntentState::Failed:               return "Failed";
        case IntentState::Recovering:           return "Recovering";
        case IntentState::CompletedWithWarnings: return "CompletedWithWarnings";
    }
    return "Unknown";
}

std::string to_string(ErrorClass ec) {
    switch (ec) {
        case ErrorClass::Transient: return "Transient";
        case ErrorClass::Permanent: return "Permanent";
        case ErrorClass::Uncertain: return "Uncertain";
        case ErrorClass::Critical:  return "Critical";
    }
    return "Unknown";
}

} // namespace tb::execution
