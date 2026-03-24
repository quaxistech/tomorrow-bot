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

} // namespace tb::opportunity_cost
