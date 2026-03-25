/**
 * @file adversarial_types.hpp
 * @brief Типы для защиты от враждебных рыночных условий
 *
 * Определяет структуры угроз, защитных действий и оценки рыночной обстановки.
 * v4: Percentile scoring, correlation matrix, multi-timeframe, hysteresis,
 *     event sourcing, auto-calibration.
 */
#pragma once

#include "common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tb::adversarial {

/// Тип обнаруженной рыночной угрозы
enum class ThreatType {
    UnstableOrderBook,      ///< Нестабильный стакан
    SpreadExplosion,        ///< Резкое расширение спреда
    SpreadVelocitySpike,    ///< Быстрое расширение спреда (скорость)
    DepthAsymmetry,         ///< Сильная асимметрия bid/ask глубины (манипуляция)
    LiquidityVacuum,        ///< Вакуум ликвидности
    ToxicFlow,              ///< Токсичный поток ордеров
    AnomalousBaseline,      ///< Z-score аномалия относительно адаптивного baseline
    BadBreakoutTrap,        ///< Ловушка ложного пробоя
    StaleMarketData,        ///< Устаревшие или несвежие рыночные данные
    InvalidMarketState,     ///< Некорректные входные рыночные данные
    PostShockCooldown,      ///< Пост-шоковое охлаждение
    ThreatEscalation,       ///< Эскалация: устойчивая серия угроз
    CorrelationBreakdown,   ///< Распад корреляции микроструктурных сигналов
    TimeframeDivergence     ///< Расхождение multi-timeframe baselines
};

/// Преобразование типа угрозы в строку
inline std::string to_string(ThreatType t) {
    switch (t) {
        case ThreatType::UnstableOrderBook:    return "UnstableOrderBook";
        case ThreatType::SpreadExplosion:      return "SpreadExplosion";
        case ThreatType::SpreadVelocitySpike:  return "SpreadVelocitySpike";
        case ThreatType::DepthAsymmetry:       return "DepthAsymmetry";
        case ThreatType::LiquidityVacuum:      return "LiquidityVacuum";
        case ThreatType::ToxicFlow:            return "ToxicFlow";
        case ThreatType::AnomalousBaseline:    return "AnomalousBaseline";
        case ThreatType::BadBreakoutTrap:      return "BadBreakoutTrap";
        case ThreatType::StaleMarketData:      return "StaleMarketData";
        case ThreatType::InvalidMarketState:   return "InvalidMarketState";
        case ThreatType::PostShockCooldown:    return "PostShockCooldown";
        case ThreatType::ThreatEscalation:     return "ThreatEscalation";
        case ThreatType::CorrelationBreakdown: return "CorrelationBreakdown";
        case ThreatType::TimeframeDivergence:  return "TimeframeDivergence";
    }
    return "Unknown";
}

/// Рекомендация защитной системы
enum class DefenseAction {
    NoAction,              ///< Без действий — безопасно
    VetoTrade,             ///< Запретить сделку
    ReduceConfidence,      ///< Снизить уверенность
    RaiseThreshold,        ///< Поднять порог входа
    Cooldown,              ///< Период охлаждения
    AlertOperator          ///< Уведомить оператора
};

/// Преобразование действия в строку
inline std::string to_string(DefenseAction a) {
    switch (a) {
        case DefenseAction::NoAction:         return "NoAction";
        case DefenseAction::VetoTrade:        return "VetoTrade";
        case DefenseAction::ReduceConfidence: return "ReduceConfidence";
        case DefenseAction::RaiseThreshold:   return "RaiseThreshold";
        case DefenseAction::Cooldown:         return "Cooldown";
        case DefenseAction::AlertOperator:    return "AlertOperator";
    }
    return "Unknown";
}

/// Классификация текущего рыночного режима
enum class MarketRegime {
    Unknown,               ///< Недостаточно данных для классификации
    Normal,                ///< Нормальные условия
    Volatile,              ///< Повышенная волатильность
    LowLiquidity,          ///< Пониженная ликвидность
    Toxic                  ///< Токсичные/враждебные условия
};

inline std::string to_string(MarketRegime r) {
    switch (r) {
        case MarketRegime::Unknown:      return "Unknown";
        case MarketRegime::Normal:       return "Normal";
        case MarketRegime::Volatile:     return "Volatile";
        case MarketRegime::LowLiquidity: return "LowLiquidity";
        case MarketRegime::Toxic:        return "Toxic";
    }
    return "Unknown";
}

/// Обнаруженная угроза
struct ThreatDetection {
    ThreatType type;
    double severity{0.0};          ///< [0=незначительная, 1=критическая]
    DefenseAction recommended_action{DefenseAction::NoAction};
    std::string reason;
    Timestamp detected_at{0};
};

/// Рыночная обстановка для анализа
struct MarketCondition {
    Symbol symbol{""};
    double spread_bps{0.0};        ///< Спред в базисных пунктах
    double book_imbalance{0.0};    ///< Дисбаланс стакана [-1, 1]
    double bid_depth{0.0};         ///< Нотиональная глубина bid-side
    double ask_depth{0.0};         ///< Нотиональная глубина ask-side
    double book_instability{0.0};  ///< Нестабильность стакана [0,1]
    double buy_sell_ratio{1.0};    ///< Отношение buy/sell объёма (>1 = доминируют покупки)
    double aggressive_flow{0.5};   ///< Доля агрессивных покупок [0,1]
    double vpin{0.0};              ///< VPIN [0,1]
    bool vpin_valid{false};
    bool spread_valid{true};
    bool liquidity_valid{true};
    bool imbalance_valid{true};
    bool instability_valid{true};
    bool flow_valid{true};
    bool book_valid{true};         ///< Валиден ли стакан
    bool market_data_fresh{true};  ///< Свеж ли feed в данный момент
    int64_t market_data_age_ns{0}; ///< Возраст входных данных в наносекундах
    std::string book_state;        ///< Качество стакана/причина деградации
    Timestamp timestamp{0};
};

/// Запись аудит-лога защитной системы (event sourcing)
struct DefenseEvent {
    int64_t timestamp_ms{0};           ///< Время события (мс)
    std::string symbol;                ///< Символ
    DefenseAction action{DefenseAction::NoAction};
    double compound_severity{0.0};
    double confidence_multiplier{1.0};
    double threshold_multiplier{1.0};
    MarketRegime regime{MarketRegime::Unknown};
    int threat_count{0};               ///< Количество обнаруженных угроз
    bool is_safe{true};
    bool hysteresis_active{false};     ///< Активен ли гистерезис в данный момент
};

/// Метрики калибровки (auto-calibration statistics)
struct CalibrationMetrics {
    int64_t total_assessments{0};       ///< Всего оценок
    int64_t veto_count{0};              ///< Сколько раз сработал VetoTrade
    int64_t cooldown_count{0};          ///< Сколько раз сработал Cooldown
    int64_t raise_threshold_count{0};   ///< RaiseThreshold
    int64_t reduce_confidence_count{0}; ///< ReduceConfidence
    int64_t safe_count{0};              ///< Безопасные тики
    double avg_compound_severity{0.0};  ///< Средняя compound severity
    double max_compound_severity{0.0};  ///< Макс. compound severity
    double veto_rate{0.0};              ///< Доля veto от всех оценок
    int64_t hysteresis_activations{0};  ///< Сколько раз вошли в hysteresis zone
    int64_t hysteresis_deactivations{0};///< Сколько раз вышли из hysteresis zone
    /// Per-threat type counts
    int64_t spread_explosion_count{0};
    int64_t liquidity_vacuum_count{0};
    int64_t toxic_flow_count{0};
    int64_t depth_asymmetry_count{0};
    int64_t correlation_breakdown_count{0};
    int64_t timeframe_divergence_count{0};
    int64_t anomalous_baseline_count{0};
    int64_t escalation_count{0};
};

/// Результат оценки защитной системы
struct DefenseAssessment {
    Symbol symbol{""};
    bool is_safe{true};                        ///< Безопасно ли торговать
    double confidence_multiplier{1.0};          ///< Множитель уверенности [0,1]
    double threshold_multiplier{1.0};           ///< Множитель порога [1, +inf)
    double compound_severity{0.0};              ///< Совокупная severity
    std::vector<ThreatDetection> threats;       ///< Обнаруженные угрозы
    DefenseAction overall_action{DefenseAction::NoAction};
    Timestamp assessed_at{0};
    bool cooldown_active{false};
    bool in_recovery{false};
    int64_t cooldown_remaining_ms{0};
    MarketRegime regime{MarketRegime::Unknown};
    double threat_memory_severity{0.0};
    bool baseline_warm{false};
    bool hysteresis_active{false};              ///< Гистерезис: текущее состояние danger zone
    double percentile_severity{0.0};            ///< Severity из percentile scoring [0,1]
};

/// Диагностика внутреннего состояния защитной системы (для мониторинга)
struct DefenseDiagnostics {
    std::string symbol;
    MarketRegime regime{MarketRegime::Unknown};
    double threat_memory_severity{0.0};
    bool baseline_warm{false};
    int64_t baseline_samples{0};
    double spread_ema{0.0};
    double spread_z{0.0};
    double depth_ema{0.0};
    double depth_z{0.0};
    double ratio_ema{1.0};
    double ratio_z{0.0};
    int consecutive_threats{0};
    int consecutive_safe{0};
    bool cooldown_active{false};
    bool in_recovery{false};
    int64_t cooldown_remaining_ms{0};
    // v4 additions
    bool hysteresis_active{false};
    double spread_depth_correlation{0.0};
    double spread_flow_correlation{0.0};
    double depth_flow_correlation{0.0};
    double fast_spread_ema{0.0};
    double slow_spread_ema{0.0};
    double spread_percentile{0.0};        ///< Текущий перцентиль спреда
    CalibrationMetrics calibration;        ///< Метрики калибровки
};

/// Конфигурация защитной системы
struct DefenseConfig {
    bool enabled{true};
    bool fail_closed_on_invalid_data{true};
    bool auto_cooldown_on_veto{true};
    double auto_cooldown_severity{0.85};
    double spread_explosion_threshold_bps{100.0};
    double spread_normal_bps{20.0};
    double min_liquidity_depth{50.0};
    double book_imbalance_threshold{0.8};
    double book_instability_threshold{0.7};
    double toxic_flow_ratio_threshold{1.8};
    double aggressive_flow_threshold{0.8};
    double vpin_toxic_threshold{0.7};
    int64_t cooldown_duration_ms{30000};
    int64_t post_shock_cooldown_ms{60000};
    int64_t max_market_data_age_ns{2'000'000'000};
    double max_confidence_reduction{0.8};
    double max_threshold_expansion{2.0};

    // --- Compound threat & recovery ---
    double compound_threat_factor{0.5};
    double cooldown_severity_scale{1.5};
    int64_t recovery_duration_ms{10000};
    double recovery_confidence_floor{0.6};

    // --- Spread velocity ---
    double spread_velocity_threshold_bps_per_sec{50.0};

    // --- Adaptive baseline ---
    double baseline_alpha{0.01};
    int64_t baseline_warmup_ticks{200};
    double z_score_spread_threshold{3.0};
    double z_score_depth_threshold{3.0};
    double z_score_ratio_threshold{3.0};
    int64_t baseline_stale_reset_ms{300'000};

    // --- Threat memory ---
    double threat_memory_alpha{0.15};
    double threat_memory_residual_factor{0.3};
    int threat_escalation_ticks{5};
    double threat_escalation_boost{0.1};

    // --- Depth asymmetry ---
    double depth_asymmetry_threshold{0.3};

    // --- Cross-signal amplification ---
    double cross_signal_amplification{0.3};

    // --- v4: Percentile scoring ---
    int percentile_window_size{500};           ///< Размер скользящего окна для перцентилей
    double percentile_severity_threshold{0.95}; ///< Перцентиль, начиная с которого severity > 0

    // --- v4: Correlation matrix ---
    double correlation_alpha{0.02};             ///< EMA alpha для rolling correlation
    double correlation_breakdown_threshold{0.4}; ///< Порог |Δcorrelation| для breakdown

    // --- v4: Time-weighted EMA & Multi-timeframe ---
    double baseline_halflife_fast_ms{30'000.0};   ///< Fast baseline: ~30с
    double baseline_halflife_medium_ms{300'000.0}; ///< Medium baseline: ~5мин
    double baseline_halflife_slow_ms{1'800'000.0}; ///< Slow baseline: ~30мин
    double timeframe_divergence_threshold{2.5};    ///< Z-score расхождение fast vs slow

    // --- v4: Hysteresis ---
    double hysteresis_enter_severity{0.5};     ///< Вход в danger zone
    double hysteresis_exit_severity{0.25};     ///< Выход из danger zone
    double hysteresis_confidence_penalty{0.15}; ///< Штраф confidence в danger zone

    // --- v4: Event sourcing ---
    int64_t audit_log_max_size{10'000};        ///< Макс. записей в аудит-логе (ring buffer)
};

} // namespace tb::adversarial
