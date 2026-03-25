/**
 * @file adversarial_types.hpp
 * @brief Типы для защиты от враждебных рыночных условий
 *
 * Определяет структуры угроз, защитных действий и оценки рыночной обстановки.
 */
#pragma once

#include "common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tb::adversarial {

/// Тип обнаруженной рыночной угрозы
enum class ThreatType {
    UnstableOrderBook,     ///< Нестабильный стакан
    SpreadExplosion,       ///< Резкое расширение спреда
    SpreadVelocitySpike,   ///< Быстрое расширение спреда (скорость)
    DepthAsymmetry,        ///< Сильная асимметрия bid/ask глубины (манипуляция)
    LiquidityVacuum,       ///< Вакуум ликвидности
    ToxicFlow,             ///< Токсичный поток ордеров
    AnomalousBaseline,     ///< Z-score аномалия относительно адаптивного baseline
    BadBreakoutTrap,       ///< Ловушка ложного пробоя
    StaleMarketData,       ///< Устаревшие или несвежие рыночные данные
    InvalidMarketState,    ///< Некорректные входные рыночные данные
    PostShockCooldown,     ///< Пост-шоковое охлаждение
    ThreatEscalation       ///< Эскалация: устойчивая серия угроз (temporal pattern)
};

/// Преобразование типа угрозы в строку
inline std::string to_string(ThreatType t) {
    switch (t) {
        case ThreatType::UnstableOrderBook:   return "UnstableOrderBook";
        case ThreatType::SpreadExplosion:     return "SpreadExplosion";
        case ThreatType::SpreadVelocitySpike: return "SpreadVelocitySpike";
        case ThreatType::DepthAsymmetry:      return "DepthAsymmetry";
        case ThreatType::LiquidityVacuum:     return "LiquidityVacuum";
        case ThreatType::ToxicFlow:           return "ToxicFlow";
        case ThreatType::AnomalousBaseline:   return "AnomalousBaseline";
        case ThreatType::BadBreakoutTrap:     return "BadBreakoutTrap";
        case ThreatType::StaleMarketData:     return "StaleMarketData";
        case ThreatType::InvalidMarketState:  return "InvalidMarketState";
        case ThreatType::PostShockCooldown:   return "PostShockCooldown";
        case ThreatType::ThreatEscalation:    return "ThreatEscalation";
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

/// Результат оценки защитной системы
struct DefenseAssessment {
    Symbol symbol{""};
    bool is_safe{true};                        ///< Безопасно ли торговать
    double confidence_multiplier{1.0};          ///< Множитель уверенности [0,1]
    double threshold_multiplier{1.0};           ///< Множитель порога [1, +inf)
    double compound_severity{0.0};              ///< Совокупная severity с учётом взаимодействия угроз
    std::vector<ThreatDetection> threats;       ///< Обнаруженные угрозы
    DefenseAction overall_action{DefenseAction::NoAction};
    Timestamp assessed_at{0};
    bool cooldown_active{false};               ///< Активен ли период охлаждения
    bool in_recovery{false};                   ///< В фазе post-cooldown recovery
    int64_t cooldown_remaining_ms{0};          ///< Оставшееся время охлаждения (мс)
    MarketRegime regime{MarketRegime::Unknown}; ///< Текущий рыночный режим
    double threat_memory_severity{0.0};         ///< EMA-smoothed severity (temporal)
    bool baseline_warm{false};                 ///< Достаточно ли данных для adaptive baseline
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
};

/// Конфигурация защитной системы
struct DefenseConfig {
    bool enabled{true};
    bool fail_closed_on_invalid_data{true};
    bool auto_cooldown_on_veto{true};
    double auto_cooldown_severity{0.85};          ///< Severity, начиная с которой ставим cooldown
    double spread_explosion_threshold_bps{100.0};  ///< Порог спреда (bps)
    double spread_normal_bps{20.0};                ///< Нормальный спред (bps)
    double min_liquidity_depth{50.0};              ///< Мин. нотиональная глубина ликвидности
    double book_imbalance_threshold{0.8};          ///< Порог |дисбаланса| стакана [-1,1]
    double book_instability_threshold{0.7};        ///< Порог нестабильности стакана
    double toxic_flow_ratio_threshold{1.8};        ///< Порог buy/sell ratio
    double aggressive_flow_threshold{0.8};         ///< Порог агрессивного потока [0,1]
    double vpin_toxic_threshold{0.7};              ///< Порог VPIN [0,1]
    int64_t cooldown_duration_ms{30000};           ///< Длительность охлаждения (30с)
    int64_t post_shock_cooldown_ms{60000};         ///< Охлаждение после шока (60с)
    int64_t max_market_data_age_ns{2'000'000'000}; ///< Макс. допустимый возраст market data
    double max_confidence_reduction{0.8};          ///< Макс. снижение confidence [0,1]
    double max_threshold_expansion{2.0};           ///< Насколько можно поднять порог входа

    // --- Compound threat & recovery ---
    double compound_threat_factor{0.5};            ///< Сила компаундинга множественных угроз [0,1]
    double cooldown_severity_scale{1.5};           ///< Множитель: duration * severity * scale
    int64_t recovery_duration_ms{10000};           ///< Продолжительность post-cooldown recovery (мс)
    double recovery_confidence_floor{0.6};         ///< Мин. confidence_multiplier во время recovery

    // --- Spread velocity ---
    double spread_velocity_threshold_bps_per_sec{50.0}; ///< Порог скорости расширения спреда

    // --- Adaptive baseline ---
    double baseline_alpha{0.01};                   ///< EMA скорость адаптации (~100 тиков half-life)
    int64_t baseline_warmup_ticks{200};            ///< Мин. тиков до доверия baseline
    double z_score_spread_threshold{3.0};          ///< Z-score порог для спреда
    double z_score_depth_threshold{3.0};           ///< Z-score порог для глубины
    double z_score_ratio_threshold{3.0};           ///< Z-score порог для buy/sell ratio
    int64_t baseline_stale_reset_ms{300'000};      ///< Сброс baseline при >5 мин без данных

    // --- Threat memory ---
    double threat_memory_alpha{0.15};              ///< EMA скорость threat memory
    double threat_memory_residual_factor{0.3};     ///< Влияние memory на confidence при отсутствии текущих угроз
    int threat_escalation_ticks{5};                ///< Количество consecutive threat тиков до эскалации
    double threat_escalation_boost{0.1};           ///< Прирост severity за каждый тик эскалации

    // --- Depth asymmetry ---
    double depth_asymmetry_threshold{0.3};         ///< min(bid,ask)/max(bid,ask) < threshold → угроза

    // --- Cross-signal amplification ---
    double cross_signal_amplification{0.3};        ///< Сила усиления при опасных комбинациях сигналов
};

} // namespace tb::adversarial
