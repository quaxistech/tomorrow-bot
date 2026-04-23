#pragma once
#include "risk/risk_types.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::risk {

/// Venue health metrics — for venue-risk protection
struct VenueHealthContext {
    double rest_avg_latency_ms{0.0};        ///< Avg REST latency over last minute
    double rest_p99_latency_ms{0.0};        ///< P99 REST latency over last minute
    int ws_reconnect_count_last_hour{0};    ///< WebSocket reconnects in last hour
    double reject_rate_last_minute{0.0};    ///< [0,1] fraction of orders rejected
    double cancel_fill_ratio{0.0};          ///< Cancel-to-fill ratio (>3 = possible issue)
    bool position_snapshot_stale{false};    ///< Position data is stale or inconsistent
    int64_t last_fill_age_ns{0};            ///< Time since last fill (for gap detection)
    double clock_drift_ms{0.0};             ///< Absolute clock drift from exchange
};

/// Margin state for margin-aware sizing
struct MarginContext {
    double margin_ratio{0.0};               ///< Current margin ratio (used/available)
    double liquidation_price{0.0};          ///< Estimated liquidation price
    double mark_price{0.0};                 ///< Current mark price
    double maintenance_margin_rate{0.005};  ///< Maintenance margin rate (0.5% default)
    double distance_to_liquidation_pct{100.0}; ///< % distance from mark to liquidation
    double available_margin{0.0};           ///< Available margin for new positions
    bool hedge_active{false};               ///< Is hedge position active?
    double combined_margin_usage_pct{0.0};  ///< Combined primary+hedge margin as % of capital
};

/// Контекст для оценки риска — объединяет все входные данные для IRiskCheck
struct RiskContext {
    const strategy::TradeIntent& intent;
    const portfolio_allocator::SizingResult& sizing;
    const portfolio::PortfolioSnapshot& portfolio;
    const features::FeatureSnapshot& features;
    const execution_alpha::ExecutionAlphaResult& exec_alpha;
    const uncertainty::UncertaintySnapshot& uncertainty;
    double current_funding_rate{0.0};  ///< Текущий funding rate (8ч)
    double min_notional_usdt{0.0};     ///< Per-symbol минимальный notional

    // Phase 5: venue-risk and margin awareness
    VenueHealthContext venue_health{};
    MarginContext margin{};
};

} // namespace tb::risk
