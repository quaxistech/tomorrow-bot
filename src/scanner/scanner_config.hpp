#pragma once
/**
 * @file scanner_config.hpp
 * @brief Конфигурация модуля сканирования рынка (§13).
 */

#include <string>
#include <vector>

namespace tb::scanner {

/// Полная конфигурация сканера (§13 — все пороги в конфиге)
///
/// Параметры по умолчанию основаны на:
/// - Aldridge (2013) "High-Frequency Trading: A Practical Guide" — vol/spread sweet spot
/// - Cartea, Jaimungal, Penalva (2015) "Algorithmic and HFT" — microstructure thresholds
/// - Empirical crypto perpetual futures data (Binance/Bitget 2023–2025)
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
    int api_timeout_ms{5000};
    int api_retry_max{3};
    int api_retry_base_delay_ms{200};

    // ── Pre-filter (before detailed analysis) ──
    double prefilter_min_volume_usdt{500'000.0};
    double prefilter_max_spread_bps{100.0};
    int max_candidates_detailed{30};

    // ── Filter thresholds (§7) ──
    double min_volume_usdt{1'000'000.0};
    double max_spread_bps{20.0};              // scalping: 20 bps max (Cartea et al. §4.3)
    double min_open_interest_usdt{500'000.0};
    double min_orderbook_depth_usdt{50'000.0};
    double max_trap_risk{0.7};
    double max_noise_level{0.8};
    double min_volatility_pct{0.10};           // realized vol < 0.1% on 5m = dead instrument
    double max_volatility_pct{8.0};            // realized vol > 8% on 5m = dangerous noise
    double min_price_usdt{0.005};              // reject micro-price contracts below $0.005
    double max_tick_value_bps{10.0};           // reject if single tick > 10 bps (poor discrete economics)
    double filter_min_change_24h{-20.0};       // min 24h price change % (futures: allow drops)
    double filter_max_change_24h{20.0};        // max 24h price change % (exhausted pump filter)

    // ── Ranking weights (§8) — positive weights sum to 1.0 ──
    double weight_liquidity{0.25};
    double weight_spread{0.20};
    double weight_volatility{0.20};            // was 0.15; increased for scalping relevance
    double weight_orderbook{0.15};
    double weight_trend_quality{0.10};
    double weight_execution_quality{0.10};     // was 0.05; execution = critical for scalping
    double weight_trap_risk{0.20};             // penalty weight
    double weight_funding_extreme{0.10};       // penalty weight

    // ── Trap detector thresholds (§6) ──
    /// Single order > 25% of 10-level depth is suspicious (SEC Market Quality rpt 2019)
    double spoofing_wall_pct{0.25};
    /// Depth asymmetry threshold — bid vs ask notional imbalance
    double spoofing_asymmetry_threshold{0.60};
    /// Wick > 70% of candle range = stop-hunt signature (Easley & O'Hara model)
    double stop_hunt_wick_ratio{0.70};
    /// Chop ratio: > 60% direction changes over 20 candles = noise market
    double noise_chop_threshold{0.60};
    /// Reversal must retrace > 50% of impulse to flag as momentum trap
    double momentum_trap_reversal_pct{0.50};
    /// Funding rate: 0.1% per 8h = extreme positioning (Gu 2023, crypto funding studies)
    /// Normal funding on Bitget: ±0.01% (0.0001). Elevated: >0.05% (0.0005).
    /// At 0.1% (0.001): strong mean-reversion signal confirmed by empirical data.
    double funding_extreme_threshold{0.001};
    /// Book instability: gap between levels in bps
    double book_instability_gap_bps{20.0};
    /// Book instability: minimum near-mid depth in USDT
    double book_instability_thin_depth_usdt{10'000.0};

    // ── Trade state thresholds ──
    double trade_state_max_trap_risk{0.6};
    double trade_state_min_score{0.3};
    double trade_state_min_confidence{0.4};

    // ── Bias detector (§9) ──
    double bias_min_confidence{0.55};
    double bias_neutral_zone{0.30};

    // ── Feature params ──
    int volatility_window{14};
    double depth_near_mid_pct{0.10};   // ±0.1% from mid for depth calc

    // ── Scan timeout & circuit breaker ──
    int scan_timeout_ms{60'000};           ///< Max duration for entire scan() call
    int circuit_breaker_threshold{5};      ///< Consecutive API failures before tripping
    int circuit_breaker_reset_ms{300'000}; ///< Time before circuit breaker resets

    // ── Basket diversification ──
    bool enable_diversification{true};          ///< Apply correlation/concentration constraints
    double max_correlation_in_basket{0.85};     ///< Max Pearson correlation between any pair of returns
    int max_pairs_per_sector{2};                ///< Max pairs from same token "sector" (L1/L2/meme/etc)

    // ── Logging ──
    bool log_detailed_analysis{false};
    bool log_rejected_pairs{true};
};

} // namespace tb::scanner
