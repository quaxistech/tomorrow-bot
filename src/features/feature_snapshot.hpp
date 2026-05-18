#pragma once
#include "common/types.hpp"
#include "order_book/order_book_types.hpp"

namespace tb::features {

// Технические индикаторы на основе свечных данных
struct TechnicalFeatures {
    double sma_20{0.0};
    double ema_8{0.0};
    double ema_20{0.0};
    double ema_50{0.0};
    bool sma_valid{false};
    bool ema_valid{false};

    double rsi_14{0.0};
    bool rsi_valid{false};

    double macd_line{0.0};
    double macd_signal{0.0};
    double macd_histogram{0.0};
    bool macd_valid{false};

    double bb_upper{0.0};
    double bb_middle{0.0};
    double bb_lower{0.0};
    double bb_bandwidth{0.0};
    double bb_percent_b{0.0};
    bool bb_valid{false};

    double atr_14{0.0};
    double atr_14_normalized{0.0};
    bool atr_valid{false};

    double adx{0.0};
    double plus_di{0.0};
    double minus_di{0.0};
    bool adx_valid{false};

    double obv{0.0};
    double directional_volume_proxy{0.0}; ///< Normalized OBV — local volume-direction proxy
    bool obv_valid{false};

    double volatility_5{0.0};
    double volatility_20{0.0};
    bool volatility_valid{false};

    double momentum_5{0.0};
    double momentum_20{0.0};
    bool momentum_valid{false};

    // ==================== CUSUM (Cumulative Sum) ====================
    /// CUSUM статистика для раннего обнаружения смены режима
    double cusum_positive{0.0};        ///< Накопленное положительное отклонение
    double cusum_negative{0.0};        ///< Накопленное отрицательное отклонение
    double cusum_threshold{0.0};       ///< Порог срабатывания (адаптивный)
    bool cusum_regime_change{false};   ///< Сигнал смены режима
    bool cusum_valid{false};

    // ==================== Volume Profile ====================
    /// Профиль объёма — распределение торгов по ценовым уровням
    double vp_poc{0.0};                ///< Point of Control — цена с макс объёмом
    double vp_value_area_high{0.0};    ///< Верхняя граница Value Area (70%)
    double vp_value_area_low{0.0};     ///< Нижняя граница Value Area (70%)
    double vp_price_vs_poc{0.0};       ///< Расстояние текущей цены от POC (-1..+1)
    bool vp_valid{false};

    // ==================== Time-of-Day ====================
    /// Внутридневной сезонный профиль
    int session_hour_utc{-1};          ///< Текущий час UTC (0-23)
    double tod_volatility_mult{1.0};   ///< Множитель волатильности для текущего часа
    double tod_volume_mult{1.0};       ///< Множитель объёма для текущего часа
    double tod_alpha_score{0.0};       ///< Альфа-оценка для текущего часа (-1..+1)
    bool tod_valid{false};

    // ==================== run93: Supertrend / Stochastic / EMA pair ============
    /// Supertrend (Olson 2008) — ATR-based trend follower. Hard trend filter.
    double supertrend_value{0.0};      ///< Текущий supertrend уровень
    int supertrend_trend{0};           ///< +1 uptrend / -1 downtrend / 0 unknown
    bool supertrend_flipped{false};    ///< Тик когда тренд развернулся
    bool supertrend_valid{false};

    /// Stochastic Oscillator (Lane 1957) — short-TF overbought/oversold.
    double stoch_k{50.0};              ///< %K [0..100]
    double stoch_d{50.0};              ///< %D [0..100]
    bool stoch_overbought{false};      ///< %K > 80
    bool stoch_oversold{false};        ///< %K < 20
    bool stoch_bull_cross{false};      ///< Bullish crossover в oversold
    bool stoch_bear_cross{false};      ///< Bearish crossover в overbought
    bool stoch_valid{false};

    /// Fast EMA pair (9/21) — micro-trend crossover.
    double ema_fast_9{0.0};            ///< EMA(9) — fast
    double ema_slow_21{0.0};           ///< EMA(21) — slow
    int ema_pair_trend{0};             ///< +1 fast > slow / -1 fast < slow
    bool ema_pair_bull_cross{false};   ///< Just crossed bullish
    bool ema_pair_bear_cross{false};   ///< Just crossed bearish
    double ema_pair_separation_bps{0.0};  ///< |fast-slow|/price × 10000
    bool ema_pair_valid{false};

    // ==================== run94: Advanced professional indicators ===============
    /// Anchored VWAP (Carter 2012) — session/daily anchor + 1σ/2σ bands.
    double avwap{0.0};
    double avwap_upper_1sigma{0.0};
    double avwap_lower_1sigma{0.0};
    double avwap_upper_2sigma{0.0};
    double avwap_lower_2sigma{0.0};
    double avwap_price_vs_vwap_bps{0.0};  ///< (price-vwap)/vwap × 10000
    bool avwap_valid{false};

    /// CVD (Cumulative Volume Delta) — taker buy vs sell pressure.
    double cvd{0.0};
    double cvd_change_recent{0.0};
    double cvd_normalized{0.0};         ///< [-1..+1] normalized by total volume
    bool cvd_bullish_divergence{false}; ///< Price LL but CVD HL
    bool cvd_bearish_divergence{false}; ///< Price HH but CVD LH
    bool cvd_valid{false};

    /// Open Interest tracking.
    double oi_current{0.0};
    double oi_change_recent_pct{0.0};
    int oi_trend_quadrant{0};           ///< 1-4 (Wyckoff)
    bool oi_valid{false};

    /// Liquidity Sweep Detector.
    bool liq_sweep_high{false};         ///< Wicked above recent high, closed back
    bool liq_sweep_low{false};          ///< Wicked below recent low, closed back
    double liq_sweep_recovery_pct{0.0};
    bool liq_sweep_valid{false};

    /// Liquidation cluster proxy.
    double liq_upside_cluster_pct{0.0};
    double liq_downside_cluster_pct{0.0};
    double liq_cascade_risk_score{0.0};  ///< 0..1
    int liq_dominant_side{0};            ///< +1 longs at risk, -1 shorts at risk
    bool liq_valid{false};

    /// Funding bias.
    double funding_rate_8h{0.0};
    int funding_crowding_side{0};        ///< +1 longs crowded, -1 shorts crowded
    double funding_crowding_intensity{0.0};  ///< 0..1
    int funding_recommended_bias{0};     ///< Mean-revert direction
    bool funding_valid{false};

    /// Spoof detection enhanced.
    bool spoof_bid{false};
    bool spoof_ask{false};
    double spoof_intensity{0.0};
    bool spoof_valid{false};
};

// Микроструктурные признаки стакана и потока сделок
struct MicrostructureFeatures {
    double spread{0.0};
    double spread_bps{0.0};
    bool spread_valid{false};

    double book_imbalance_5{0.0};
    double book_imbalance_10{0.0};
    bool book_imbalance_valid{false};

    double weighted_mid_price{0.0};
    double mid_price{0.0};

    double buy_sell_ratio{1.0};
    double aggressive_flow{0.5};
    double trade_vwap{0.0};
    bool trade_flow_valid{false};

    double bid_depth_5_notional{0.0};
    double ask_depth_5_notional{0.0};
    double liquidity_ratio{1.0};
    bool liquidity_valid{false};

    double book_instability{0.0};
    bool instability_valid{false};

    // ==================== VPIN ====================
    /// Volume-Synchronized Probability of Informed Trading
    double vpin{0.0};                  ///< VPIN значение [0..1], >0.7 = токсичный поток
    double vpin_canonical{0.0};        ///< Canonical VPIN (фиксированный бакет, Easley et al. 2012)
    double vpin_adaptive{0.0};         ///< Adaptive VPIN (рекалибруемый бакет)
    double vpin_ma{0.0};               ///< Скользящее среднее VPIN
    bool vpin_toxic{false};            ///< Флаг токсичного потока (VPIN > порог)
    bool vpin_valid{false};

    // ==================== Event-Time Features (Phase 6) ====================
    double top_of_book_churn{0.0};     ///< Частота смены best bid/ask [0..1]
    double cancel_burst_intensity{0.0};///< Доля removals в event window [0..1]
    bool cancel_burst_active{false};   ///< Burst выше порога
    double queue_depletion_bid{0.0};   ///< Скорость истощения bid-стороны
    double queue_depletion_ask{0.0};   ///< Скорость истощения ask-стороны
    double refill_asymmetry{0.0};      ///< (bid_refills - ask_refills)/total [-1..1]
    bool event_features_valid{false};  ///< Достаточно событий для event-time features

    // ==================== Execution Quality Feedback ====================
    double passive_fill_rate{0.0};     ///< Limit fills / total limit orders
    double cancel_to_fill_ratio{0.0};  ///< Cancels / fills
    double adverse_selection_bps{0.0}; ///< Средний adverse move после fill (bps)
    bool execution_feedback_valid{false};
};

// Контекст исполнения — оценка условий для открытия позиции
struct ExecutionContextFeatures {
    double spread_cost_bps{0.0};
    double immediate_liquidity{0.0};
    double estimated_slippage_bps{0.0};
    bool slippage_valid{false};
    bool is_market_open{true};
    bool is_feed_fresh{false};
};

// Полный снимок признаков для одного символа в момент времени
struct FeatureSnapshot {
    tb::Symbol symbol{""};
    tb::Timestamp computed_at{0};
    tb::Timestamp market_data_age_ns{0};

    tb::Price last_price{0.0};
    tb::Price mid_price{0.0};

    TechnicalFeatures technical;
    MicrostructureFeatures microstructure;
    ExecutionContextFeatures execution_context;

    tb::order_book::BookQuality book_quality{tb::order_book::BookQuality::Uninitialized};

    // Снимок считается полным, если доступны минимально необходимые для
    // скальпинговых стратегий признаки: тренд (SMA), волатильность (ATR) и спред.
    // Без ATR невозможна корректная оценка stop-loss / take-profit,
    // без спреда — execution cost модель.
    [[nodiscard]] bool is_complete() const noexcept {
        return technical.sma_valid
            && technical.atr_valid
            && microstructure.spread_valid;
    }
};

} // namespace tb::features
