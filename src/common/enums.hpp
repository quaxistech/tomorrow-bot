/**
 * @file enums.hpp
 * @brief Утилиты преобразования перечислений в строки и обратно
 * 
 * Предоставляет to_string() и from_string() для всех перечислений
 * из types.hpp. Используется при сериализации конфигурации,
 * логировании и экспорте метрик.
 */
#pragma once

#include "types.hpp"
#include "errors.hpp"
#include "result.hpp"
#include <string_view>
#include <optional>

namespace tb {

// ============================================================
// to_string для всех перечислений
// ============================================================

[[nodiscard]] constexpr std::string_view to_string(Side v) noexcept {
    switch (v) {
        case Side::Buy:  return "buy";
        case Side::Sell: return "sell";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType v) noexcept {
    switch (v) {
        case OrderType::Limit:       return "limit";
        case OrderType::Market:      return "market";
        case OrderType::PostOnly:    return "post_only";
        case OrderType::StopMarket:  return "stop_market";
        case OrderType::StopLimit:   return "stop_limit";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce v) noexcept {
    switch (v) {
        case TimeInForce::GoodTillCancel:    return "gtc";
        case TimeInForce::ImmediateOrCancel: return "ioc";
        case TimeInForce::FillOrKill:        return "fok";
        case TimeInForce::GoodTillDate:      return "gtd";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OrderStatus v) noexcept {
    switch (v) {
        case OrderStatus::Pending:          return "pending";
        case OrderStatus::Open:             return "open";
        case OrderStatus::PartiallyFilled:  return "partially_filled";
        case OrderStatus::Filled:           return "filled";
        case OrderStatus::Cancelled:        return "cancelled";
        case OrderStatus::Rejected:         return "rejected";
        case OrderStatus::Expired:          return "expired";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(TradingMode v) noexcept {
    switch (v) {
        case TradingMode::Paper:      return "paper";
        case TradingMode::Shadow:     return "shadow";
        case TradingMode::Testnet:    return "testnet";
        case TradingMode::Production: return "production";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(RegimeLabel v) noexcept {
    switch (v) {
        case RegimeLabel::Trending:  return "trending";
        case RegimeLabel::Ranging:   return "ranging";
        case RegimeLabel::Volatile:  return "volatile";
        case RegimeLabel::Unclear:   return "unclear";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(WorldStateLabel v) noexcept {
    switch (v) {
        case WorldStateLabel::Stable:        return "stable";
        case WorldStateLabel::Transitioning: return "transitioning";
        case WorldStateLabel::Disrupted:     return "disrupted";
        case WorldStateLabel::Unknown:       return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(UncertaintyLevel v) noexcept {
    switch (v) {
        case UncertaintyLevel::Low:      return "low";
        case UncertaintyLevel::Moderate: return "moderate";
        case UncertaintyLevel::High:     return "high";
        case UncertaintyLevel::Extreme:  return "extreme";
    }
    return "unknown";
}

// ============================================================
// from_string для всех перечислений (возвращает std::optional)
// ============================================================

[[nodiscard]] inline std::optional<TradingMode> trading_mode_from_string(std::string_view s) noexcept {
    if (s == "paper")      return TradingMode::Paper;
    if (s == "shadow")     return TradingMode::Shadow;
    if (s == "testnet")    return TradingMode::Testnet;
    if (s == "production") return TradingMode::Production;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<Side> side_from_string(std::string_view s) noexcept {
    if (s == "buy")  return Side::Buy;
    if (s == "sell") return Side::Sell;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<OrderType> order_type_from_string(std::string_view s) noexcept {
    if (s == "limit")       return OrderType::Limit;
    if (s == "market")      return OrderType::Market;
    if (s == "post_only")   return OrderType::PostOnly;
    if (s == "stop_market") return OrderType::StopMarket;
    if (s == "stop_limit")  return OrderType::StopLimit;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<OrderStatus> order_status_from_string(std::string_view s) noexcept {
    if (s == "pending")          return OrderStatus::Pending;
    if (s == "open")             return OrderStatus::Open;
    if (s == "partially_filled") return OrderStatus::PartiallyFilled;
    if (s == "filled")           return OrderStatus::Filled;
    if (s == "cancelled")        return OrderStatus::Cancelled;
    if (s == "rejected")         return OrderStatus::Rejected;
    if (s == "expired")          return OrderStatus::Expired;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<RegimeLabel> regime_from_string(std::string_view s) noexcept {
    if (s == "trending")  return RegimeLabel::Trending;
    if (s == "ranging")   return RegimeLabel::Ranging;
    if (s == "volatile")  return RegimeLabel::Volatile;
    if (s == "unclear")   return RegimeLabel::Unclear;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<UncertaintyLevel> uncertainty_from_string(std::string_view s) noexcept {
    if (s == "low")      return UncertaintyLevel::Low;
    if (s == "moderate") return UncertaintyLevel::Moderate;
    if (s == "high")     return UncertaintyLevel::High;
    if (s == "extreme")  return UncertaintyLevel::Extreme;
    return std::nullopt;
}

} // namespace tb
