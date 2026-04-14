#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::world_model {

// ============================================================
// Пороги классификации состояний мира
//
// Все дефолтные значения обоснованы научной литературой и
// эмпирикой USDT-M perpetual futures (Binance/Bitget):
//   - Wilder (1978): ADX >25 = trending, <20 = non-trending
//   - Wilder (1978): RSI 80/20 = strong overbought/oversold
//   - Bollinger (2001): %B ≈ 1.0/0.0 = band touch, BW squeeze
//   - Easley, López de Prado, O'Hara (2012): VPIN, toxic flow
//   - Cont, Kukanov, Stoikov (2014): order book dynamics
//   - Empirical USDT-M: BTC spread 0.5-3 bps normal,
//     15+ bps = microstructure stress, 50+ bps = vacuum
// ============================================================

/// Пороги для ToxicMicrostructure
///   Cont et al. (2014): book_instability >0.7 = unstable LOB
///   Easley/O'Hara: aggressive_flow >0.8 = informed trading dominance
///   USDT-M empirical: spread >15 bps = abnormal for major pairs
struct ToxicMicrostructureThresholds {
    double book_instability_min{0.7};
    double aggressive_flow_min{0.8};
    double spread_bps_min{15.0};
};

/// Пороги для LiquidityVacuum
///   USDT-M empirical: spread >50 bps = critical (5-10x normal)
///   spread >20 bps + liquidity_ratio <0.3 = depth-confirmed vacuum
struct LiquidityVacuumThresholds {
    double spread_bps_critical{50.0};      ///< Спред → безусловный вакуум
    double spread_bps_secondary{20.0};     ///< Спред + перекос → вакуум
    double liquidity_ratio_min{0.3};       ///< bid_depth/ask_depth floor
};

/// Пороги для ExhaustionSpike
///   Wilder (1978): RSI >80/<20 = сильный overbought/oversold
///   momentum >2% = значительное 5-bar ценовое изменение
struct ExhaustionSpikeThresholds {
    double rsi_upper{80.0};
    double rsi_lower{20.0};
    double momentum_abs_min{0.02};
};

/// Пороги для FragileBreakout
///   Bollinger (2001): %B >0.95/<0.05 = выход за границы полос
///   vol_5 >2% + book imbalance >0.3 = подтверждение пробоя
struct FragileBreakoutThresholds {
    double bb_percent_b_upper{0.95};
    double bb_percent_b_lower{0.05};
    double volatility_5_min{0.02};
    double book_imbalance_abs_min{0.3};
};

/// Пороги для CompressionBeforeExpansion
///   Bollinger Squeeze: BW <3% = исторически узкие полосы
///   ATR/price <1% и vol_5 <1% = минимальная реализованная волатильность
struct CompressionThresholds {
    double bb_bandwidth_max{0.03};
    double atr_normalized_max{0.01};
    double volatility_5_max{0.01};
};

/// Пороги для StableTrendContinuation
///   Wilder (1978): ADX >25 = наличие тренда
///   RSI [40,70] = здоровый тренд без перекупленности
struct StableTrendThresholds {
    double adx_min{25.0};
    double rsi_lower{40.0};
    double rsi_upper{70.0};
};

/// Пороги для ChopNoise
///   Wilder (1978): ADX <20 = отсутствие тренда
///   RSI [40,60] = нейтральная зона
///   USDT-M: spread <20 bps = нормальная ликвидность (не вакуум)
struct ChopNoiseThresholds {
    double adx_max{20.0};
    double rsi_lower{40.0};
    double rsi_upper{60.0};
    double spread_bps_max{20.0};
};

// ============================================================
// Конфигурация хрупкости (composite fragility scoring)
//
// Fragility отражает вероятность резкого неблагоприятного движения.
// Базовые значения: из эмпирики USDT-M perpetuals (BTC/ETH/ALT).
// Веса компонент: нормализованы так, что max adjustment ≈ 0.82,
// т.е. base + adjustment клампится в [0, 1].
//
// Источники:
//   - Bouchaud et al. (2018): Price Impact, Order Flow
//   - Easley et al. (2012): VPIN flow toxicity metric
//   - Cont, Kukanov, Stoikov (2014): LOB stability → instability >0.7
// ============================================================

struct FragilityConfig {
    /// Базовая хрупкость каждого состояния [0,1]
    double stable_trend{0.2};       ///< Тренд устойчив, низкая fragility
    double fragile_breakout{0.8};   ///< Пробой по определению хрупок
    double compression{0.6};        ///< Сжатие → средне, неясен исход
    double chop_noise{0.3};         ///< Боковик стабилен, но не информативен
    double exhaustion_spike{0.9};   ///< Спайк истощения → крайне хрупок
    double liquidity_vacuum{0.95};  ///< Вакуум → максимальная опасность
    double toxic_micro{0.85};       ///< Токсичная микроструктура
    double post_shock{0.5};         ///< Стабилизация → средняя хрупкость
    double unknown{0.5};            ///< Нет данных → консервативная оценка

    /// Веса компонент composite fragility (сумма ≈ 0.82)
    double spread_stress_weight{0.15};       ///< spread_bps / normalization
    double spread_normalization{100.0};      ///< USDT-M: 100 bps = полный стресс
    double book_instability_weight{0.20};    ///< Cont (2014): LOB instability
    double volatility_accel_weight{0.12};    ///< |vol_5 − vol_20| / vol_20
    double liquidity_imbalance_weight{0.10}; ///< 1 − liquidity_ratio
    double transition_instability_weight{0.15}; ///< частые смены состояний
    double vpin_toxicity_weight{0.10};       ///< Easley (2012): VPIN

    /// Клампинг итоговой хрупкости
    double floor{0.0};
    double ceiling{1.0};
};

// ============================================================
// Конфигурация персистентности
//
// Персистентность [0,1] = вероятность остаться в текущем состоянии.
// Базовые значения: эмпирика USDT-M (BTC/ETH 1s–5s тики).
//   Hamilton (1989): regime-switching models — persist ≈ 0.95–0.99
//   для дневных баров; для внутридневных тиков значения ниже.
// ============================================================

struct PersistenceConfig {
    double stable_trend{0.75};       ///< Тренды удерживаются хорошо
    double fragile_breakout{0.30};   ///< Пробои быстро подтверждаются или откатываются
    double compression{0.60};        ///< Сжатие может длиться долго
    double chop_noise{0.75};         ///< Боковик самоподдерживается
    double exhaustion_spike{0.20};   ///< Спайки кратковременны
    double liquidity_vacuum{0.20};   ///< Вакуум разрешается быстро
    double toxic_micro{0.40};        ///< Токсичность может удерживаться
    double post_shock{0.50};         ///< Стабилизация — переходная фаза
    double unknown{0.50};

    /// Вес эмпирической оценки из истории переходов
    double history_blend_weight{0.3};
    size_t min_history_for_empirical{10};
};

// ============================================================
// Гистерезис
//
// Hamilton (1989): regime-switching requires confirmation ticks
// to avoid false transitions on noise.
//   confirmation_ticks = 2: стандарт для HFT/скальпинга
//   min_dwell_ticks = 3: минимальная "жизнь" состояния
// ============================================================

struct HysteresisConfig {
    int confirmation_ticks{2};          ///< Тиков до подтверждения нового состояния
    int min_dwell_ticks{3};             ///< Минимальное время в текущем состоянии
    bool enabled{true};
};

// ============================================================
// История и буфер состояний
// ============================================================

struct HistoryConfig {
    size_t max_entries{200};             ///< Максимальный размер ring-буфера
    size_t tendency_lookback{5};         ///< Глубина для оценки тенденции
};

// ============================================================
// Стратегическая пригодность
// ============================================================

/// Одна ячейка таблицы пригодности: state × strategy → suitability
struct SuitabilityEntry {
    std::string strategy_id;
    double suitability{0.3};
    std::string reason;
};

/// Конфигурация системы пригодности стратегий (USDT-M futures / скальпинг)
struct SuitabilityConfig {
    /// Таблица state → vector<SuitabilityEntry>
    /// Ключ = индекс WorldState (0..8)
    std::unordered_map<int, std::vector<SuitabilityEntry>> table;

    /// Множитель из feedback loop (performance-adjusted)
    double feedback_blend_weight{0.3};
    size_t min_trades_for_feedback{20};  ///< Статистический минимум для значимости

    /// Граница veto: если suitability ниже → стратегия блокируется
    double hard_veto_threshold{0.10};

    /// Создаёт таблицу пригодности по умолчанию
    static SuitabilityConfig make_default();
};

// ============================================================
// Конфигурация feedback engine
//
// EMA alpha = 0.05 → эффективное окно ~40 сделок (2/α).
// При частоте 1 сделка/5мин это ~3.3 часа адаптации.
// ============================================================

struct FeedbackConfig {
    bool enabled{true};              ///< Активировать feedback loop
    double ema_alpha{0.05};          ///< Скорость обновления EMA-статистик
};

// ============================================================
// Корневая конфигурация WorldModel (USDT-M perpetual futures)
// ============================================================

struct WorldModelConfig {
    /// Версия модели для audit trail
    std::string model_version{"2.0.0"};

    /// Пороги классификации
    ToxicMicrostructureThresholds toxic;
    LiquidityVacuumThresholds liquidity_vacuum;
    ExhaustionSpikeThresholds exhaustion;
    FragileBreakoutThresholds fragile_breakout;
    CompressionThresholds compression;
    StableTrendThresholds stable_trend;
    ChopNoiseThresholds chop_noise;

    /// Подсистемы
    FragilityConfig fragility;
    PersistenceConfig persistence;
    HysteresisConfig hysteresis;
    HistoryConfig history;
    SuitabilityConfig suitability;
    FeedbackConfig feedback;

    /// Минимальное количество валидных индикаторов для классификации.
    /// Для USDT-M скальпинга требуется как минимум: spread + trend + vol + micro = 4.
    int min_valid_indicators{4};

    /// Создаёт конфигурацию по умолчанию
    static WorldModelConfig make_default();

    /// Валидация: все пороги в допустимых пределах
    [[nodiscard]] bool validate() const {
        if (stable_trend.adx_min <= 0.0 || stable_trend.adx_min > 100.0) return false;
        if (chop_noise.adx_max <= 0.0 || chop_noise.adx_max > 100.0) return false;
        if (exhaustion.rsi_upper <= exhaustion.rsi_lower) return false;
        if (liquidity_vacuum.spread_bps_critical <= 0.0) return false;
        if (hysteresis.confirmation_ticks < 0) return false;
        if (history.max_entries == 0) return false;
        if (min_valid_indicators < 1 || min_valid_indicators > 12) return false;
        if (fragility.spread_normalization <= 0.0) return false;
        if (feedback.ema_alpha <= 0.0 || feedback.ema_alpha > 1.0) return false;
        return true;
    }
};

} // namespace tb::world_model
