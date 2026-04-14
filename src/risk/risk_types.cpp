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

std::string to_string(RiskAction action) {
    switch (action) {
        case RiskAction::Allow:               return "Allow";
        case RiskAction::AllowWithReducedSize: return "AllowWithReducedSize";
        case RiskAction::Deny:                return "Deny";
        case RiskAction::DenySymbolLock:       return "DenySymbolLock";
        case RiskAction::DenyStrategyLock:     return "DenyStrategyLock";
        case RiskAction::DenyDayLock:          return "DenyDayLock";
        case RiskAction::DenyAccountLock:      return "DenyAccountLock";
        case RiskAction::EmergencyHalt:        return "EmergencyHalt";
    }
    return "Unknown";
}

std::string to_string(RiskStateLevel level) {
    switch (level) {
        case RiskStateLevel::Normal:        return "Normal";
        case RiskStateLevel::Degraded:      return "Degraded";
        case RiskStateLevel::DayLock:       return "DayLock";
        case RiskStateLevel::SymbolLock:    return "SymbolLock";
        case RiskStateLevel::StrategyLock:  return "StrategyLock";
        case RiskStateLevel::AccountLock:   return "AccountLock";
        case RiskStateLevel::EmergencyHalt: return "EmergencyHalt";
    }
    return "Unknown";
}

std::string to_string(LockType type) {
    switch (type) {
        case LockType::SymbolLock:     return "SymbolLock";
        case LockType::StrategyLock:   return "StrategyLock";
        case LockType::DayLock:        return "DayLock";
        case LockType::AccountLock:    return "AccountLock";
        case LockType::EmergencyHalt:  return "EmergencyHalt";
        case LockType::Cooldown:       return "Cooldown";
    }
    return "Unknown";
}

} // namespace tb::risk
