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

} // namespace tb::risk
