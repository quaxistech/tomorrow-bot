#include "risk/risk_types.hpp"

namespace tb::risk {

std::string to_string(RiskVerdict verdict) {
    switch (verdict) {
        case RiskVerdict::Approved:   return "Approved";
        case RiskVerdict::Denied:     return "Denied";
        case RiskVerdict::ReduceSize: return "ReduceSize";
        case RiskVerdict::Throttled:  return "Throttled";
    }
    return "Unknown";
}

std::string to_string(RiskPhase phase) {
    switch (phase) {
        case RiskPhase::PreTrade:   return "PreTrade";
        case RiskPhase::IntraTrade: return "IntraTrade";
        case RiskPhase::PostTrade:  return "PostTrade";
    }
    return "Unknown";
}

} // namespace tb::risk
