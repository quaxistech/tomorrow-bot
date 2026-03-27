#pragma once
#include <cstddef>

namespace tb::strategy {

/// Конфигурация стратегии Breakout (Bollinger Band сжатие→расширение)
struct BreakoutConfig {
    std::size_t bandwidth_history_size{10};  ///< Размер буфера BB bandwidth
    double compression_threshold{0.03};      ///< Порог сжатия BB bandwidth
    double expansion_ratio{1.5};             ///< Коэффициент расширения от минимума
    double adx_min{20.0};                    ///< Мин. ADX для подтверждения тренда
    double adx_strong{30.0};                 ///< ADX для бонуса conviction
    double obv_block_threshold{0.0};         ///< OBV ниже этого блокирует BUY
    double obv_surge_threshold{0.5};         ///< OBV выше этого даёт бонус
    double base_conviction{0.3};             ///< Базовая conviction
    double max_conviction{0.90};             ///< Максимальная conviction
    double momentum_bonus{0.15};             ///< Бонус за momentum
    double adx_bonus{0.10};                  ///< Бонус за сильный ADX
    double volume_bonus{0.10};               ///< Бонус за OBV surge
    bool allow_counter_trend{false};         ///< Разрешить BUY против медвежьего тренда
    double counter_trend_conviction_mult{0.5}; ///< Множитель conviction при контр-тренде
};

/// Конфигурация стратегии Momentum (EMA crossover + ADX + acceleration)
struct MomentumConfig {
    double adx_min{20.0};                    ///< Мин. ADX для тренда
    double momentum_threshold{0.005};        ///< Мин. |momentum_5| (0.5%)
    double rsi_overbought{75.0};             ///< RSI выше = перекуплен (блок BUY)
    double rsi_oversold{25.0};               ///< RSI ниже = перепродан (блок SELL)
    double accel_threshold{0.001};           ///< Порог ускорения momentum
    double obv_confirm_threshold{0.3};       ///< Мин. OBV для подтверждения
    double base_conviction{0.35};            ///< Базовая conviction
    double max_conviction{0.95};             ///< Максимальная conviction
    double adx_weight{0.25};                 ///< Вес ADX в conviction
    double momentum_weight{0.20};            ///< Вес momentum в conviction
    double accel_max_bonus{0.15};            ///< Макс. бонус за ускорение
    double obv_bonus{0.05};                  ///< Бонус за подтверждение OBV
    bool volatility_scale_momentum{true};    ///< Масштабировать порог momentum по ATR
};

/// Конфигурация стратегии Mean Reversion (BB + RSI extremes)
struct MeanReversionConfig {
    double bb_buy_threshold{0.35};           ///< BUY если bb_pos < X
    double bb_sell_threshold{0.75};          ///< SELL если bb_pos > X
    double rsi_buy_threshold{43.0};          ///< BUY если RSI < X
    double rsi_sell_threshold{60.0};         ///< SELL если RSI > X
    double rsi_panic_low{15.0};              ///< RSI < X = зона паники (блок BUY)
    double rsi_euphoria_high{85.0};          ///< RSI > X = эйфория (блок SELL)
    double adx_block_threshold{40.0};        ///< ADX > X = слишком сильный тренд
    double adx_strong_trend{30.0};           ///< ADX > X = сильный тренд (фильтр)
    double macd_bonus_mult{1.25};            ///< Множитель conviction при MACD confirm
    double base_conviction{0.25};            ///< Базовая conviction
    double max_conviction{0.95};             ///< Максимальная conviction
    double rsi_weight{0.35};                 ///< Вес RSI в conviction
    double bb_weight{0.25};                  ///< Вес BB position в conviction
    bool allow_panic_entry{false};           ///< Разрешить вход при RSI < panic
    double panic_conviction_mult{0.5};       ///< Множитель conviction при panic entry
};

/// Конфигурация стратегии Microstructure Scalp
struct MicrostructureScalpConfig {
    double imbalance_threshold{0.25};        ///< Мин. |book_imbalance_5| для сигнала
    double buy_sell_ratio_buy{1.3};          ///< Мин. buy_sell_ratio для BUY
    double buy_sell_ratio_sell{0.77};        ///< Макс. buy_sell_ratio для SELL
    double max_spread_bps{10.0};             ///< Макс. спред для скальпинга
    double rsi_upper_guard{75.0};            ///< RSI > X = блок BUY
    double rsi_lower_guard{25.0};            ///< RSI < X = блок SELL
    double base_conviction{0.30};            ///< Базовая conviction
    double max_conviction{0.85};             ///< Максимальная conviction
    double trend_bonus{0.10};                ///< Бонус за тренд в направлении сигнала
    double strong_seller_bonus{0.05};        ///< Бонус за сильное доминирование
    double limit_price_spread_frac{0.25};    ///< Доля спреда для лимитной цены
    bool block_counter_trend{true};          ///< Блокировать сигнал против тренда
    bool allow_reduced_counter_trend{false}; ///< Разрешить контр-тренд с пониженной conviction
    double vpin_soft_scale{true};            ///< Мягкое масштабирование по VPIN вместо hard block
};

/// Конфигурация стратегии Vol Expansion (ATR compression→expansion)
struct VolExpansionConfig {
    std::size_t atr_history_size{10};        ///< Размер буфера ATR
    double compression_ratio{0.70};          ///< older_avg < recent_avg * X = was compressed
    double expansion_rate_min{0.30};         ///< Мин. expansion rate
    double adx_min{20.0};                    ///< Мин. ADX для подтверждения
    double momentum_threshold{0.003};        ///< Мин. |momentum_5| для направления
    double base_conviction{0.30};            ///< Базовая conviction
    double max_conviction{0.95};             ///< Максимальная conviction
    double expansion_weight{0.25};           ///< Вес expansion rate в conviction
    double adx_weight{0.25};                 ///< Вес ADX в conviction
    double momentum_weight{0.15};            ///< Вес momentum в conviction
};

/// Конфигурация стратегии EMA Pullback (тренд + откат)
struct EmaPullbackConfig {
    double adx_min{22.0};                    ///< Мин. ADX для подтверждения тренда
    double pullback_depth_min{0.3};          ///< Мин. глубина отката (нормализованная)
    double pullback_depth_max{0.8};          ///< Макс. глубина отката (дальше = тренд сломан)
    double rsi_buy_min{35.0};                ///< Мин. RSI для BUY pullback
    double rsi_buy_max{55.0};                ///< Макс. RSI для BUY pullback
    double momentum_recovery_threshold{0.0}; ///< Momentum > X = восстановление началось
    double base_conviction{0.30};            ///< Базовая conviction
    double max_conviction{0.90};             ///< Максимальная conviction
    double ema_proximity_weight{0.30};       ///< Вес близости к EMA
    double rsi_weight{0.25};                 ///< Вес RSI в conviction
    double adx_weight{0.20};                 ///< Вес ADX в conviction
};

/// Конфигурация стратегии RSI Divergence (ценовые/индикаторные расхождения)
struct RsiDivergenceConfig {
    std::size_t lookback_bars{20};           ///< Окно поиска расхождений
    double price_new_extreme_pct{0.001};     ///< Мин. новый экстремум (0.1%)
    double rsi_divergence_min{2.0};          ///< Мин. RSI дивергенция (пункты)
    double rsi_oversold{35.0};               ///< RSI < X = зона перепроданности для bullish div
    double rsi_overbought{65.0};             ///< RSI > X = зона перекупленности для bearish div
    double adx_max{35.0};                    ///< ADX > X = тренд слишком силён (блок)
    double macd_confirm_weight{0.15};        ///< Бонус за MACD подтверждение
    double base_conviction{0.30};            ///< Базовая conviction
    double max_conviction{0.85};             ///< Максимальная conviction
    double divergence_strength_weight{0.30}; ///< Вес силы дивергенции
    double rsi_depth_weight{0.25};           ///< Вес глубины RSI от extrema
};

/// Конфигурация стратегии VWAP Reversion (возврат к VWAP)
struct VwapReversionConfig {
    double deviation_entry_pct{0.003};       ///< Мин. отклонение от VWAP (0.3%)
    double deviation_max_pct{0.015};         ///< Макс. отклонение (дальше — тренд)
    double rsi_buy_max{45.0};                ///< RSI < X для BUY (ниже VWAP)
    double rsi_sell_min{55.0};               ///< RSI > X для SELL (выше VWAP)
    double adx_max{30.0};                    ///< ADX > X = слишком трендовый для reversion
    double volume_confirm_threshold{0.3};    ///< Мин. OBV confirm
    double base_conviction{0.28};            ///< Базовая conviction
    double max_conviction{0.85};             ///< Максимальная conviction
    double deviation_weight{0.35};           ///< Вес отклонения от VWAP
    double rsi_weight{0.25};                 ///< Вес RSI
    double volume_weight{0.15};              ///< Вес volume confirmation
};

/// Конфигурация стратегии Volume Profile Reversion (POC/VA levels)
struct VolumeProfileConfig {
    double poc_proximity_pct{0.005};         ///< Мин. расстояние от POC (0.5%)
    double va_proximity_pct{0.003};          ///< Мин. расстояние от VA границ (0.3%)
    double rsi_confirm_buy{45.0};            ///< RSI < X для подтверждения BUY
    double rsi_confirm_sell{55.0};           ///< RSI > X для подтверждения SELL
    double adx_max{35.0};                    ///< ADX > X = тренд слишком силён
    double base_conviction{0.28};            ///< Базовая conviction
    double max_conviction{0.85};             ///< Максимальная conviction
    double poc_weight{0.35};                 ///< Вес близости к POC
    double va_weight{0.25};                  ///< Вес близости к VA
    double rsi_weight{0.20};                 ///< Вес RSI
};

} // namespace tb::strategy
