#pragma once
#include "common/types.hpp"
#include "order_book/order_book_types.hpp"

namespace tb::features {

// Технические индикаторы на основе свечных данных
struct TechnicalFeatures {
    double sma_20{0.0};
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
    double obv_normalized{0.0};
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
    double vpin_ma{0.0};               ///< Скользящее среднее VPIN
    bool vpin_toxic{false};            ///< Флаг токсичного потока (VPIN > порог)
    bool vpin_valid{false};
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
