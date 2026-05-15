#include "opportunity_cost/opportunity_cost_types.hpp"

namespace tb::opportunity_cost {

std::string to_string(OpportunityAction action) {
    switch (action) {
        case OpportunityAction::Execute:  return "Execute";
        case OpportunityAction::Defer:    return "Defer";
        case OpportunityAction::Suppress: return "Suppress";
        case OpportunityAction::Upgrade:  return "Upgrade";
    }
    return "Unknown";
}

std::string to_string(OpportunityReason reason) {
    switch (reason) {
        case OpportunityReason::None:                      return "None";
        case OpportunityReason::NegativeNetEdge:           return "NegativeNetEdge";
        case OpportunityReason::ConvictionBelowThreshold:  return "ConvictionBelowThreshold";
        case OpportunityReason::HighExposureLowConviction: return "HighExposureLowConviction";
        case OpportunityReason::InsufficientNetEdge:       return "InsufficientNetEdge";
        case OpportunityReason::CapitalExhausted:          return "CapitalExhausted";
        case OpportunityReason::HighConcentration:         return "HighConcentration";
        case OpportunityReason::StrongEdgeAvailable:       return "StrongEdgeAvailable";
        case OpportunityReason::HighConvictionOverride:    return "HighConvictionOverride";
        case OpportunityReason::UpgradeBetterCandidate:    return "UpgradeBetterCandidate";
        case OpportunityReason::DefaultDefer:              return "DefaultDefer";
    }
    return "Unknown";
}

} // namespace tb::opportunity_cost
