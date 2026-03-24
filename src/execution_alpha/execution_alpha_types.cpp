#include "execution_alpha/execution_alpha_types.hpp"

namespace tb::execution_alpha {

std::string to_string(ExecutionStyle style) {
    switch (style) {
        case ExecutionStyle::Passive:     return "Passive";
        case ExecutionStyle::Aggressive:  return "Aggressive";
        case ExecutionStyle::Hybrid:      return "Hybrid";
        case ExecutionStyle::PostOnly:    return "PostOnly";
        case ExecutionStyle::NoExecution: return "NoExecution";
    }
    return "Unknown";
}

} // namespace tb::execution_alpha
