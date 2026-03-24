#pragma once
/**
 * @file ai_advisory_types.hpp
 * @brief Типы AI Advisory — ML-рекомендации и правиловый анализ
 */
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::ai {

/// Уровень уверенности AI
enum class AIConfidenceLevel { Low, Medium, High };

/// Тип рекомендации
enum class AIRecommendationType {
    RegimeInsight,       ///< Наблюдение о режиме
    AnomalyWarning,      ///< Предупреждение об аномалии
    ConfidenceAdjust,    ///< Корректировка уверенности
    StrategyHint,        ///< Рекомендация по стратегии
    RiskWarning          ///< Предупреждение о риске
};

[[nodiscard]] inline std::string to_string(AIRecommendationType t) {
    switch (t) {
        case AIRecommendationType::RegimeInsight:    return "RegimeInsight";
        case AIRecommendationType::AnomalyWarning:   return "AnomalyWarning";
        case AIRecommendationType::ConfidenceAdjust: return "ConfidenceAdjust";
        case AIRecommendationType::StrategyHint:     return "StrategyHint";
        case AIRecommendationType::RiskWarning:      return "RiskWarning";
    }
    return "Unknown";
}

[[nodiscard]] inline std::string to_string(AIConfidenceLevel l) {
    switch (l) {
        case AIConfidenceLevel::Low:    return "Low";
        case AIConfidenceLevel::Medium: return "Medium";
        case AIConfidenceLevel::High:   return "High";
    }
    return "Unknown";
}

/// Рекомендация AI Advisory
struct AIAdvisory {
    AIRecommendationType type{AIRecommendationType::RegimeInsight};
    AIConfidenceLevel confidence_level{AIConfidenceLevel::Low};
    double confidence_adjustment{0.0};   ///< Корректировка [-0.5, +0.5]
    std::string insight;                  ///< Текстовое наблюдение
    std::string regime_suggestion;        ///< Предложение по режиму
    std::string anomaly_explanation;      ///< Объяснение аномалии
    std::vector<std::string> tags;        ///< Метки
    Timestamp generated_at{Timestamp(0)};
    int64_t processing_time_ms{0};        ///< Время обработки (мс)
    bool is_available{false};             ///< Backward compat field
};

/// Конфигурация AI Advisory
struct AIAdvisoryConfig {
    bool enabled{false};                  ///< Включён ли AI
    int timeout_ms{2000};                 ///< Таймаут (мс)
    double max_confidence_adjustment{0.3}; ///< Макс. корректировка
};

/// Статус AI-сервиса
struct AIServiceStatus {
    bool available{false};
    bool healthy{false};
    int requests_total{0};
    int requests_failed{0};
    Timestamp last_success{0};
};

} // namespace tb::ai
