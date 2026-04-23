#pragma once
/**
 * @file strategy_config.hpp
 * @brief Конфигурация Strategy Engine — единственной скальпинговой стратегии (§26 ТЗ)
 */

#include <cstdint>

namespace tb::strategy {

/// Полная конфигурация Strategy Engine
struct ScalpStrategyConfig {
    // ─── Порог сетапа (§13-14) ────────────────────────────────────────────
    double min_setup_confidence{0.45};
    double max_trap_risk_for_entry{0.6};
    double max_spread_bps_for_entry{25.0};
    double min_liquidity_ratio{0.3};
    double min_expected_move_to_spread_ratio{2.0};

    // ─── Таймауты (§14-15) ───────────────────────────────────────────────
    int64_t setup_confirmation_window_ms{3000};
    int64_t setup_timeout_ms{15000};
    int64_t cooldown_after_exit_ms{5000};
    int64_t cooldown_after_failed_setup_ms{3000};
    // max_hold_time_ms removed: all exits are market-driven via continuation value model

    // ─── Под-сценарии (§10) ──────────────────────────────────────────────
    bool enable_momentum_continuation{true};
    bool enable_retest_scenarios{true};
    bool enable_pullback_scenarios{true};
    bool enable_rejection_scenarios{true};

    // ─── Вход (§14) ─────────────────────────────────────────────────────
    bool allow_reentry_after_stop{false};
    bool block_counter_trend{true};

    // ─── Микроструктура (§13) ────────────────────────────────────────────
    double imbalance_threshold{0.08};
    double buy_sell_ratio_buy{1.08};
    double buy_sell_ratio_sell{0.92};
    double adx_min{15.0};
    double vpin_toxic_threshold{0.7};

    // ─── RSI / Bollinger Guards ──────────────────────────────────────────
    double rsi_upper_guard{80.0};
    double rsi_lower_guard{20.0};
    double bb_max_buy{1.50};
    double bb_min_sell{-0.50};
    double momentum_min_buy{-0.003};
    double momentum_max_sell{0.003};

    // ─── Conviction (§7) ─────────────────────────────────────────────────
    double base_conviction{0.50};
    double max_conviction{0.95};
    double trend_bonus{0.15};
    double strong_seller_bonus{0.08};
    double setup_confirmation_bonus{0.10};
    double multi_factor_bonus{0.05};

    // ─── Сопровождение позиции (§17) ─────────────────────────────────────
    bool reduce_on_structure_degradation{true};
    bool exit_on_microtrend_failure{true};
    bool exit_on_trap_detection{true};
    double reduce_fraction{0.5};

    // ─── Лимитные цены ───────────────────────────────────────────────────
    double limit_price_spread_frac{0.25};

    // ─── Пулбэк параметры ────────────────────────────────────────────────
    double pullback_min_trend_strength{0.5};
    double pullback_max_depth_pct{0.015};

    // ─── Ретест параметры ────────────────────────────────────────────────
    double retest_level_tolerance_pct{0.002};

    // ─── Rejection параметры ─────────────────────────────────────────────
    double rejection_bb_extreme{0.85};
    double rejection_min_reversal_momentum{0.002};
};

} // namespace tb::strategy
