#pragma once
/**
 * @file scanner_config.hpp
 * @brief Конфигурация модуля сканирования рынка (§13).
 */

#include <string>
#include <vector>

namespace tb::scanner {

/// EDGE-31 Scalping profile (run82 2026-05-15): hard gates + soft weights под scalp.
/// Pairs failing hard_gates auto-rejected. Soft weights ranking among survivors.
struct ScalpingProfile {
    bool enabled{true};   // false = legacy ranking, true = scalp-optimized

    // Hard gates (cliff cutoffs — reject). RELAXED v2 (run82 audit): only 4 pairs prošli,
    // нужно больше activity. Loosen marginal gates но keep critical (slippage/wick).
    double hg_max_spread_bps{15.0};               // 8→15: more pairs, fee still covered
    double hg_min_book_depth_1pct_usdt{25'000.0}; // 50k→25k: alts realistic
    double hg_max_slippage_at_10usdt_bps{5.0};    // 3→5: $10 order не критично
    double hg_min_trade_count_1m{20.0};           // 50→20: lower bar для activity
    double hg_min_resiliency{0.3};                // 0.4→0.3: после take recovers
    double hg_max_funding_abs{0.001};             // 0.0005→0.001: standard 0.1% per 8h
    double hg_min_realized_vol_1m_pct{0.08};      // 0.10→0.08: lower bar для liveness
    double hg_max_realized_vol_1m_pct{2.0};       // 1.5→2.0
    double hg_max_wick_ratio{0.70};               // 0.60→0.70: still anti-stop-hunt

    // Soft weights (composite ranking among survivors, sum ≈ 1.0)
    double w_spread{0.20};
    double w_depth{0.15};
    double w_resiliency{0.12};
    double w_vol_quality{0.15};
    double w_trade_flow{0.12};
    double w_micro_structure{0.10};
    double w_execution_quality{0.10};
    double w_regime_match{0.06};

    // Penalties
    double p_trap_risk{0.30};
    double p_funding_drift{0.15};
    double p_btc_correlation{0.10};

    // Vol sweet spot для score (peak score @ 0.30%)
    double vol_sweet_spot_pct{0.30};
};

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
    double rotation_interval_hours{4.0};   ///< EDGE-15: double для fractional intervals (e.g. 0.17 = 10min)

    // ── Data collection ──
    int orderbook_depth{20};
    int candle_count{50};
    std::string candle_interval{"5m"};
    int api_timeout_ms{5000};
    int api_retry_max{3};
    int api_retry_base_delay_ms{200};

    // ── Pre-filter (before detailed analysis) ──
    // run103: 500k → 300k для max coverage.
    double prefilter_min_volume_usdt{300'000.0};
    double prefilter_max_spread_bps{100.0};
    int max_candidates_detailed{30};

    // ── Filter thresholds (§7) ──
    double min_volume_usdt{1'000'000.0};
    double max_spread_bps{20.0};              // scalping: 20 bps max (Cartea et al. §4.3)
    double min_open_interest_usdt{500'000.0};
    // run100 calibration: 30k → 20k — micro_min_orderbook_depth_usdt доминирует
    // (для micro account override). 20k = original baseline.
    double min_orderbook_depth_usdt{20'000.0};
    double max_trap_risk{0.85};                  ///< 0.70 → 0.85 (allow moderate manipulation)
    double max_noise_level{0.85};                ///< 0.80 → 0.85
    double min_volatility_pct{0.10};           // realized vol < 0.1% on 5m = dead instrument
    double max_volatility_pct{8.0};            // realized vol > 8% on 5m = dangerous noise
    double min_price_usdt{0.005};              // reject micro-price contracts below $0.005
    double max_tick_value_bps{10.0};           // reject if single tick > 10 bps (poor discrete economics)
    double filter_min_change_24h{-20.0};       // min 24h price change % (futures: allow drops)
    double filter_max_change_24h{20.0};        // max 24h price change % (exhausted pump filter)

    // BUG-EDGE-9 (prop-momentum hunter mode): rebalance toward high-vol trending movers.
    // Bias scanner на real momentum candidates (high ATR + trend), away from slow majors.
    double weight_liquidity{0.15};             // 0.25 → 0.15 (don't bias toward slow majors)
    double weight_spread{0.20};                 // keep — costs critical
    double weight_volatility{0.30};             // 0.20 → 0.30 (find movers)
    double weight_orderbook{0.10};              // 0.15 → 0.10
    double weight_trend_quality{0.20};          // 0.10 → 0.20 — TREND IS KING
    double weight_execution_quality{0.05};      // 0.10 → 0.05
    double weight_trap_risk{0.20};              // keep — avoid manipulated
    double weight_funding_extreme{0.10};        // keep

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
    // run102: 0.40 → 0.30 — user хочет 15-20 pairs в watchlist.
    // Финальная защита от bad setups через Bayesian fusion / freshness / NetRR gates
    // в strategy + execution path. Scanner — только pre-screening.
    double trade_state_min_score{0.30};
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

    // ── Micro-account scaling (D12 fix) ──
    /// Капитал ниже этого порога активирует ослабленные пороги ликвидности/OI/trap.
    /// Защита от ситуации, когда ордера ботa ($5–$10) не проходят строгие сетевые
    /// фильтры рассчитанные на $1K+ позиции. Применяется в `apply_capital_scaling`.
    double micro_account_capital_threshold_usdt{100.0};
    /// run100 calibration: 8k → 5k — pre-filter rejected 91/100 pairs, лучше дать
    /// больше pairs пройти hard gates, фильтровать по composite score (trade_state).
    double micro_min_orderbook_depth_usdt{5'000.0};
    /// Ослабленный порог Open Interest для микро-аккаунта.
    /// run101 calibration: 50k → 25k для micro — больше алткоинов пройдёт hard gate.
    /// Бот торгует $5-10 ордерами, не нужны $500k+ OI pairs.
    double micro_min_open_interest_usdt{25'000.0};

    // ── EDGE-31 Scalping profile ──
    ScalpingProfile scalping;
};

} // namespace tb::scanner
