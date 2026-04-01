#pragma once
/**
 * @file scanner_config.hpp
 * @brief Конфигурация модуля сканирования рынка (§13).
 */

#include <string>
#include <vector>

namespace tb::scanner {

/// Полная конфигурация сканера (§13 — все пороги в конфиге)
struct ScannerConfig {
    // ── Universe ──
    int top_n{5};
    std::vector<std::string> whitelist;
    std::vector<std::string> blacklist;
    std::string product_type{"USDT-FUTURES"};

    // ── Scan timing ──
    int scan_interval_seconds{300};
    int rotation_interval_hours{4};

    // ── Data collection ──
    int orderbook_depth{20};
    int candle_count{50};
    std::string candle_interval{"5m"};
    int max_concurrent_requests{5};
    int api_timeout_ms{5000};
    int api_retry_max{3};
    int api_retry_base_delay_ms{200};

    // ── Pre-filter (before detailed analysis) ──
    double prefilter_min_volume_usdt{500'000.0};
    double prefilter_max_spread_bps{100.0};
    int max_candidates_detailed{30};

    // ── Filter thresholds (§7) ──
    double min_volume_usdt{1'000'000.0};
    double max_spread_bps{50.0};
    double min_open_interest_usdt{500'000.0};
    double min_orderbook_depth_usdt{50'000.0};
    double max_trap_risk{0.7};
    double max_noise_level{0.8};
    double min_volatility_pct{0.05};
    double max_volatility_pct{20.0};

    // ── Ranking weights (§8) ──
    double weight_liquidity{0.25};
    double weight_spread{0.20};
    double weight_volatility{0.15};
    double weight_orderbook{0.15};
    double weight_trend_quality{0.10};
    double weight_execution_quality{0.05};
    double weight_trap_risk{0.20};        // penalty
    double weight_funding_extreme{0.10};  // penalty

    // ── Trap detector thresholds (§6) ──
    double spoofing_wall_pct{0.30};
    double stop_hunt_wick_ratio{0.70};
    double false_breakout_reenter_pct{0.50};
    double noise_chop_threshold{0.60};
    double momentum_trap_reversal_pct{0.50};
    double funding_extreme_threshold{0.03};

    // ── Bias detector (§9) ──
    double bias_min_confidence{0.55};
    double bias_neutral_zone{0.30};

    // ── Feature params ──
    int volatility_window{14};
    double depth_near_mid_pct{0.10};   // ±0.1% from mid for depth calc

    // ── Logging ──
    bool log_detailed_analysis{false};
    bool log_rejected_pairs{true};
};

} // namespace tb::scanner
