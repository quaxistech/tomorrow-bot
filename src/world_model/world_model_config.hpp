#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::world_model {

// ============================================================
// Пороги классификации состояний мира
// ============================================================

/// Пороги для ToxicMicrostructure
struct ToxicMicrostructureThresholds {
    double book_instability_min{0.7};
    double aggressive_flow_min{0.8};
    double spread_bps_min{15.0};
};

/// Пороги для LiquidityVacuum
struct LiquidityVacuumThresholds {
    double spread_bps_critical{50.0};      ///< Спред → безусловный вакуум
    double spread_bps_secondary{20.0};     ///< Спред + перекос → вакуум
    double liquidity_ratio_min{0.3};       ///< Минимальный коэффициент ликвидности
};

/// Пороги для ExhaustionSpike
struct ExhaustionSpikeThresholds {
    double rsi_upper{80.0};
    double rsi_lower{20.0};
    double momentum_abs_min{0.02};
};

/// Пороги для FragileBreakout
struct FragileBreakoutThresholds {
    double bb_percent_b_upper{0.95};
    double bb_percent_b_lower{0.05};
    double volatility_5_min{0.02};
    double book_imbalance_abs_min{0.3};
};

/// Пороги для CompressionBeforeExpansion
struct CompressionThresholds {
    double bb_bandwidth_max{0.03};
    double atr_normalized_max{0.01};
    double volatility_5_max{0.01};
};

/// Пороги для StableTrendContinuation
struct StableTrendThresholds {
    double adx_min{25.0};
    double rsi_lower{40.0};
    double rsi_upper{70.0};
};

/// Пороги для ChopNoise
struct ChopNoiseThresholds {
    double adx_max{20.0};
    double rsi_lower{40.0};
    double rsi_upper{60.0};
    double spread_bps_max{20.0};
};

// ============================================================
// Конфигурация хрупкости
// ============================================================

struct FragilityConfig {
    /// Базовая хрупкость каждого состояния
    double stable_trend{0.2};
    double fragile_breakout{0.8};
    double compression{0.6};
    double chop_noise{0.3};
    double exhaustion_spike{0.9};
    double liquidity_vacuum{0.95};
    double toxic_micro{0.85};
    double post_shock{0.5};
    double unknown{0.5};

    /// Веса компонент composite fragility
    double spread_stress_weight{0.15};
    double spread_normalization{100.0};     ///< spread_bps / normalization
    double book_instability_weight{0.20};
    double volatility_accel_weight{0.12};   ///< |vol_5 - vol_20| / vol_20
    double liquidity_imbalance_weight{0.10};
    double transition_instability_weight{0.15};
    double vpin_toxicity_weight{0.10};

    /// Верхняя/нижняя граница итоговой хрупкости
    double floor{0.0};
    double ceiling{1.0};
};

// ============================================================
// Конфигурация персистентности
// ============================================================

struct PersistenceConfig {
    /// Базовая персистентность каждого состояния
    double stable_trend{0.75};
    double fragile_breakout{0.30};
    double compression{0.60};
    double chop_noise{0.75};
    double exhaustion_spike{0.20};
    double liquidity_vacuum{0.20};
    double toxic_micro{0.40};
    double post_shock{0.50};
    double unknown{0.50};

    /// Вес эмпирической оценки (из истории переходов)
    double history_blend_weight{0.3};
    size_t min_history_for_empirical{10};
};

// ============================================================
// Гистерезис
// ============================================================

struct HysteresisConfig {
    int confirmation_ticks{2};          ///< Тиков до подтверждения нового состояния
    int min_dwell_ticks{3};             ///< Минимальное время в текущем состоянии
    double min_confidence_to_switch{0.55}; ///< Минимальная уверенность для перехода
    bool enabled{true};
};

// ============================================================
// История и буфер состояний
// ============================================================

struct HistoryConfig {
    size_t max_entries{200};             ///< Максимальный размер ring-буфера
    size_t transition_matrix_window{100};///< Окно для матрицы переходов
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

/// Конфигурация системы пригодности
struct SuitabilityConfig {
    /// Таблица state → vector<SuitabilityEntry>
    /// Ключ = индекс WorldState (0..8)
    std::unordered_map<int, std::vector<SuitabilityEntry>> table;

    /// Множитель из feedback loop (performance-adjusted)
    double feedback_blend_weight{0.3};
    size_t min_trades_for_feedback{20};
    double alpha_decay_weight{0.2};

    /// Граница veto: если suitability ниже → стратегия блокируется
    double hard_veto_threshold{0.05};

    /// Создаёт таблицу пригодности по умолчанию
    static SuitabilityConfig make_default();
};

// ============================================================
// Режимная адаптация порогов
// ============================================================

/// Модификаторы порогов при определённом режиме рынка
struct RegimeAdjustment {
    double adx_offset{0.0};              ///< Сдвиг порога ADX
    double rsi_tolerance{0.0};           ///< Расширение RSI-диапазона
    double spread_tolerance{0.0};        ///< Допуск спреда
    double fragility_boost{0.0};         ///< Дополнительная хрупкость
};

struct RegimeAdaptationConfig {
    bool enabled{false};
    /// Ключ = DetailedRegime as int
    std::unordered_map<int, RegimeAdjustment> adjustments;
};

// ============================================================
// Конфигурация feedback engine
// ============================================================

struct FeedbackConfig {
    bool enabled{false};
    size_t min_samples_per_state{50};
    double max_threshold_drift_pct{20.0}; ///< Максимальный сдвиг порога за раз (%)
    double ema_alpha{0.05};               ///< Скорость обновления EMA-статистик
    size_t evaluation_window{500};        ///< Окно оценки (тиков)
};

// ============================================================
// Корневая конфигурация WorldModel
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
    RegimeAdaptationConfig regime_adaptation;
    FeedbackConfig feedback;

    /// Минимальное количество валидных индикаторов для классификации
    int min_valid_indicators{2};

    /// Создаёт конфигурацию по умолчанию
    static WorldModelConfig make_default();

    /// Валидация: все пороги в допустимых пределах
    [[nodiscard]] bool validate() const;
};

} // namespace tb::world_model
