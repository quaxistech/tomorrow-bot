#pragma once
/**
 * @file alpha_decay_types.hpp
 * @brief Типы мониторинга угасания альфы — метрики, алерты, отчёты
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::alpha_decay {

/// Рекомендация при обнаружении деградации
enum class DecayRecommendation {
    NoAction,           ///< Действий не требуется
    ReduceWeight,       ///< Снизить вес стратегии
    ReduceSize,         ///< Уменьшить размер позиции
    RaiseThresholds,    ///< Повысить пороги входа
    MoveToShadow,       ///< Перевести в теневой режим
    Disable,            ///< Отключить стратегию
    AlertOperator       ///< Оповестить оператора
};

/// Строковое представление рекомендации
[[nodiscard]] inline std::string to_string(DecayRecommendation r) {
    switch (r) {
        case DecayRecommendation::NoAction:        return "NoAction";
        case DecayRecommendation::ReduceWeight:    return "ReduceWeight";
        case DecayRecommendation::ReduceSize:      return "ReduceSize";
        case DecayRecommendation::RaiseThresholds: return "RaiseThresholds";
        case DecayRecommendation::MoveToShadow:    return "MoveToShadow";
        case DecayRecommendation::Disable:         return "Disable";
        case DecayRecommendation::AlertOperator:   return "AlertOperator";
    }
    return "Unknown";
}

/// Измерение деградации
enum class DecayDimension {
    Expectancy,             ///< Ожидаемая доходность
    HitRate,                ///< Процент прибыльных сделок
    SlippageAdjusted,       ///< С учётом проскальзывания
    RegimeConditioned,      ///< С учётом режима рынка
    ConfidenceReliability,  ///< Надёжность убеждённости
    ExecutionQuality,       ///< Качество исполнения
    AdverseExcursion        ///< Неблагоприятное отклонение
};

/// Строковое представление измерения
[[nodiscard]] inline std::string to_string(DecayDimension d) {
    switch (d) {
        case DecayDimension::Expectancy:            return "Expectancy";
        case DecayDimension::HitRate:               return "HitRate";
        case DecayDimension::SlippageAdjusted:      return "SlippageAdjusted";
        case DecayDimension::RegimeConditioned:     return "RegimeConditioned";
        case DecayDimension::ConfidenceReliability: return "ConfidenceReliability";
        case DecayDimension::ExecutionQuality:      return "ExecutionQuality";
        case DecayDimension::AdverseExcursion:      return "AdverseExcursion";
    }
    return "Unknown";
}

/// Результат одной сделки
struct TradeOutcome {
    double pnl_bps{0.0};           ///< P&L в базисных пунктах
    double slippage_bps{0.0};      ///< Проскальзывание (бп)
    RegimeLabel regime{RegimeLabel::Unclear}; ///< Режим рынка
    double conviction{0.0};         ///< Убеждённость модели
    Timestamp timestamp{0};         ///< Время сделки
};

/// Метрика деградации по одному измерению
struct DecayMetric {
    DecayDimension dimension{DecayDimension::Expectancy};
    double current_value{0.0};      ///< Текущее значение
    double baseline_value{0.0};     ///< Базовое (эталонное) значение
    double drift_pct{0.0};          ///< Отклонение (%)
    double z_score{0.0};            ///< Z-скор отклонения
    bool is_degraded{false};        ///< Признак деградации
    size_t lookback_trades{0};      ///< Количество сделок в окне
};

/// Алерт деградации
struct DecayAlert {
    StrategyId strategy_id{""};     ///< ID стратегии
    DecayDimension dimension{DecayDimension::Expectancy};
    DecayRecommendation recommendation{DecayRecommendation::NoAction};
    double severity{0.0};           ///< Серьёзность (0.0–1.0)
    std::string message;            ///< Описание
    Timestamp detected_at{0};       ///< Время обнаружения
};

/// Отчёт о деградации стратегии
struct AlphaDecayReport {
    StrategyId strategy_id{""};     ///< ID стратегии
    std::vector<DecayMetric> metrics;   ///< Метрики по измерениям
    std::vector<DecayAlert> alerts;     ///< Алерты
    DecayRecommendation overall_recommendation{DecayRecommendation::NoAction};
    double overall_health{1.0};     ///< Общее здоровье (0.0–1.0)
    Timestamp computed_at{0};       ///< Время вычисления
};

/// Конфигурация мониторинга деградации
struct DecayConfig {
    size_t short_lookback{20};              ///< Короткое окно (сделки)
    size_t long_lookback{100};              ///< Длинное окно (сделки)
    double expectancy_drift_threshold{0.3}; ///< Порог дрейфа ожидаемой доходности
    double hit_rate_drift_threshold{0.15};  ///< Порог дрейфа процента прибыльных
    double z_score_alert_threshold{2.0};    ///< Порог Z-скора для алерта
    double health_critical_threshold{0.3};  ///< Критический порог здоровья
    double health_warning_threshold{0.5};   ///< Предупредительный порог здоровья
};

} // namespace tb::alpha_decay
