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
    // BUG-EDGE-3 (live run13): tightened thresholds — fewer setups, but each one должен иметь
    // достаточный expected move чтобы перекрыть fees ($0.025) + spread ($0.020) + slippage ($0.015).
    // На $200 notional требуется ≥30 bps net move = ≥60 bps gross target.
    // BUG-EDGE-4 (run14 11h analysis): 0.78 заблокировал слишком много, 8bps spread исключал
    // multi-bp альтсы. Калибровка: confidence 0.65 (значимо но не overfit), spread 15bps,
    // expected_move/spread 3× (вместо 5×).
    // EDGE-15 (active trading 2026-05-14): relax conf для active opportunity hunting.
    // run89 calibration (2026-05-17): 0.62 → 0.65 — чуть строже после 3 losses.
    double min_setup_confidence{0.65};
    double max_trap_risk_for_entry{0.65};               ///< 0.55 → 0.65 (allow moderate manipulation)
    double max_spread_bps_for_entry{15.0};              ///< 12 → 15 bps
    double min_liquidity_ratio{0.3};                    ///< 0.4 → 0.3
    double min_expected_move_to_spread_ratio{2.5};      ///< 3 → 2.5 (still > fees coverage)

    // EDGE-17 (fast-reaction scalping 2026-05-14): 3000ms confirmation = 3 сек ожидания,
    // 54/126 SELL setups отменяются через imbalance_reversed в этом окне (entry latency 3.6s avg).
    // 800ms — стандарт для scalping: orderbook ещё свежий, momentum не выгорел.
    // EDGE-30-IDLE (run55 2026-05-15): setups detected но cancelled через imbalance_reversed
    // или setup_timeout. Снижаем confirmation_window до 200ms (быстрый scalp на momentum
    // без ожидания) и timeout до 3sec (если orderbook reverse — cancel fast).
    int64_t setup_confirmation_window_ms{200};   ///< 800 → 200 ms (4× faster)
    int64_t setup_timeout_ms{3000};               ///< 8000 → 3000 ms (faster expire)
    // EDGE-30 (run54 spiral 2026-05-15): 3sec cooldown → 90sec. Bot reentries на ту же монету
    // через 3 sec после ANY close — попадает на reverse. ENA-3/ICP-2/ENA-4/VIRTUAL подряд
    // -0.146 USDT за 45 минут. Нужен real anti-reentry window.
    int64_t cooldown_after_exit_ms{90000};        ///< 3000 → 90000 ms (90 sec full cooldown)
    int64_t cooldown_after_failed_setup_ms{15000}; ///< 2000 → 15000 ms
    // max_hold_time_ms removed: all exits are market-driven via continuation value model

    // BUG-EDGE-8 (live consensus): only MomentumContinuation. Pullback/Rejection counter-trend
    // gambling в noisy markets — отключены чтобы не разбавлять edge.
    bool enable_momentum_continuation{true};
    bool enable_retest_scenarios{false};         ///< true → false (low-quality setups)
    bool enable_pullback_scenarios{false};       ///< true → false (counter-trend в chop)
    bool enable_rejection_scenarios{false};      ///< true → false

    // ─── Вход (§14) ─────────────────────────────────────────────────────
    bool allow_reentry_after_stop{false};
    bool block_counter_trend{true};

    // ─── Микроструктура (§13) ────────────────────────────────────────────
    // BUG-EDGE-9 (momentum hunter): балансируем — не paralysis, но trend confirmation.
    double imbalance_threshold{0.10};       ///< 0.15 → 0.10 (более чувствительно)
    double buy_sell_ratio_buy{1.12};        ///< 1.20 → 1.12
    double buy_sell_ratio_sell{0.88};       ///< 0.83 → 0.88
    double adx_min{20.0};                   ///< 25 → 20 (medium trend OK, run15 был на 15)
    double vpin_toxic_threshold{0.7};

    // EDGE-20.2 (final calibration 2026-05-14): EDGE-20.1 bb_min_sell=0.15 блокировал
    // ВСЕ SELL signals в bearish trend (bb_pos < 0 = lower BB = норма для downtrend).
    // Лучшее решение: блокировать ТОЛЬКО при экстремальном RSI, без BB filter.
    // BB не сам по себе exhaustion в trending market.
    double rsi_upper_guard{72.0};           ///< Block BUY если RSI>72 (real overbought)
    double rsi_lower_guard{28.0};           ///< Block SELL если RSI<28 (real oversold)
    double bb_max_buy{1.10};                ///< Block BUY если bb_pos>1.10 (extreme upper only)
    double bb_min_sell{-0.10};              ///< Block SELL если bb_pos<-0.10 (extreme lower only)
    // EDGE-14 → EDGE-15 (run28 2h17m idle 0 setups at 0.25%): too strict for active trading.
    // Compromise 0.12% — 1.5× стандартного, фильтрует weak но не paralysis.
    double momentum_min_buy{0.0012};
    double momentum_max_sell{-0.0012};
    // BUG-EDGE-2: PullbackInMicrotrend требует deeper pullback (RSI confirms reversal zone).
    double pullback_rsi_buy_max{45.0};   ///< BUY pullback: RSI ≤ 45 (oversold-ish)
    double pullback_rsi_sell_min{55.0};  ///< SELL pullback: RSI ≥ 55 (overbought-ish)

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

    // ─── ATR-based stop / take-profit multipliers (§17 ТЗ, edge-31 TPSL refactor) ──
    // Stop = mid ∓ atr × stop_mult; TP = mid ± atr × target_mult.
    //
    // run95 calibration (2026-05-17): observed slippage 46 bps avg на market close.
    // SL ATR×0.8 (~70-80 bps) + slippage 40 bps = real loss 110-120 bps.
    // Решение: wider SL (atr×1.0) → меньше slippage hits, но wider TP сохраняет R:R.
    // New: SL=ATR×1.0, TP=ATR×1.8. R:R=1.8. Real loss с slippage 10 bps = 110 bps.
    // Net R:R after costs (12 bps fees + 20 bps slip total) = (180-32)/(100+10) = 1.35.
    double atr_stop_mult_momentum{1.0};
    double atr_target_mult_momentum{1.8};
    double atr_stop_mult_retest{1.2};
    double atr_target_mult_retest{2.2};
    double atr_stop_mult_pullback{1.0};
    double atr_target_mult_pullback{1.8};
    double atr_stop_mult_rejection{1.2};
    double atr_target_mult_rejection{2.2};

    // === run106 (audit fix bundle): magic numbers → config ===
    /// B1.2: fallback ATR при отсутствии setup (fraction от entry).
    double fallback_atr_fraction{0.005};
    /// B1.3: порог в bps от entry для классификации PnL win/loss.
    /// 1 bps = 0.01% — устойчиво к различным размерам позиций.
    double pnl_threshold_bps{1.0};
    /// B1.4: cooldown после rejection pipeline (ms).
    int64_t cooldown_after_rejection_ms{30000};
    /// B1.5: максимальный возраст setup при подтверждении (ns).
    int64_t max_signal_age_ns{2'000'000'000LL};
    /// B1.6: таймаут EntrySent (ms).
    int64_t entry_sent_timeout_ms{5000};
    /// B1.7: urgency для intent (для execution_alpha).
    double default_intent_urgency{0.30};
    /// B22.1: baseline volatility для adaptive thresholds.
    double adaptive_baseline_vol{0.003};
    /// B22.2: divisor для imbalance strength normalization.
    double imbalance_strength_divisor{0.5};
    /// B22.3: коэффициенты confidence boost.
    double imb_strength_weight{0.20};
    double bayesian_confidence_weight{0.25};
    /// B22.4: минимальная Bayesian posterior для пропуска фильтра.
    double bayesian_min_confidence{0.55};
    /// B22.5: Volume Profile POC проксимити и бонус.
    double poc_close_distance{0.3};
    double poc_far_distance{0.8};
    double poc_close_bonus{0.03};
    double poc_far_penalty{0.02};
    /// B22.6: добавочный ADX при SIDEWAYS HTF.
    double adx_sideways_boost{3.0};
};

} // namespace tb::strategy
