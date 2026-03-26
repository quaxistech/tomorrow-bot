#pragma once
/**
 * @file ai_advisory_types.hpp
 * @brief Типы AI Advisory — ML-рекомендации и правиловый анализ
 *
 * Все пороги вынесены в AIAdvisoryConfig для runtime-настройки через YAML.
 *
 * Состояния (AdvisoryState):
 *   Clear   — ни один детектор не сработал / все показатели ниже caution-порога.
 *   Caution — severity >= caution_severity_threshold: размер позиции умножается на
 *             caution_size_multiplier (дефолт 0.5), сделка проходит.
 *   Veto    — severity >= veto_severity_threshold: сделка блокируется полностью.
 *             Гистерезис: переход в Clear только после hysteresis_clear_ticks
 *             подряд ниже caution-порога.
 *
 * Ансамблевая эскалация:
 *   Если одновременно сработало >= ensemble_escalation_count детекторов,
 *   max_severity увеличивается на ensemble_escalation_bonus (capped at 1.0).
 *
 * Взвешенная агрегация:
 *   total_confidence_adjustment = Σ(adj_i × severity_i × confidence_weight_i) / clamped.
 *   confidence_weight: High=1.5, Medium=1.0, Low=0.7.
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace tb::ai {

/// Уровень уверенности AI
enum class AIConfidenceLevel { Low, Medium, High };

/// Состояние advisory-движка с учётом гистерезиса
enum class AdvisoryState {
    Clear,    ///< Нет активных предупреждений
    Caution,  ///< Повышенная осторожность — размер позиции снижается
    Veto      ///< Полная блокировка сделки
};

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

[[nodiscard]] inline std::string to_string(AdvisoryState s) {
    switch (s) {
        case AdvisoryState::Clear:   return "Clear";
        case AdvisoryState::Caution: return "Caution";
        case AdvisoryState::Veto:    return "Veto";
    }
    return "Unknown";
}

/// Рекомендация AI Advisory
struct AIAdvisory {
    AIRecommendationType type{AIRecommendationType::RegimeInsight};
    AIConfidenceLevel confidence_level{AIConfidenceLevel::Low};
    double confidence_adjustment{0.0};   ///< Корректировка [-0.5, +0.5]
    double severity{0.0};                ///< Серьёзность [0=info, 1=critical]
    std::string insight;                  ///< Текстовое наблюдение
    std::string regime_suggestion;        ///< Предложение по режиму
    std::string anomaly_explanation;      ///< Объяснение аномалии
    std::vector<std::string> tags;        ///< Метки
    Timestamp generated_at{Timestamp(0)};
    int64_t processing_time_ms{0};        ///< Время обработки (мс)
    bool is_available{false};             ///< Backward compat field
};

/// Агрегированный результат всех AI-рекомендаций за один тик
struct AIAdvisoryResult {
    std::vector<AIAdvisory> advisories;   ///< Все сработавшие рекомендации (отсортированы по severity)
    double total_confidence_adjustment{0.0}; ///< Агрегированная корректировка (severity-weighted, с clamping)
    double max_severity{0.0};             ///< Наивысшая серьёзность среди рекомендаций
    bool has_veto{false};                 ///< Есть ли рекомендация с severity >= veto_threshold
    int64_t processing_time_ms{0};        ///< Суммарное время обработки

    // --- Новые поля: профессиональный advisory ---
    AdvisoryState advisory_state{AdvisoryState::Clear}; ///< Состояние с учётом гистерезиса
    int ensemble_count{0};               ///< Количество одновременно сработавших детекторов
    double advisory_size_multiplier{1.0}; ///< Множитель размера позиции (1.0=норма, 0.5=caution)

    [[nodiscard]] bool empty() const { return advisories.empty(); }
    [[nodiscard]] size_t count() const { return advisories.size(); }
};

/// Конфигурируемые пороги детекторов
struct AIDetectorThresholds {
    // Volatility
    double volatility_ratio_threshold{3.0};     ///< vol5/vol20 ratio для аномалии
    double volatility_confidence_adj{-0.3};

    // RSI
    double rsi_overbought{85.0};                ///< RSI > X → перекупленность
    double rsi_oversold{15.0};                  ///< RSI < X → перепроданность
    double rsi_confidence_adj{-0.15};

    // Spread
    double spread_anomaly_bps{50.0};            ///< Спред > X bps → аномалия
    double spread_confidence_adj{-0.25};

    // Liquidity
    double liquidity_ratio_min{0.3};            ///< Ликвидность < X → предупреждение
    double liquidity_confidence_adj{-0.2};

    // VPIN Toxic Flow
    double vpin_toxic_threshold{0.7};           ///< VPIN > X → токсичный поток
    double vpin_confidence_adj{-0.25};

    // Volume Profile
    double vp_poc_deviation_threshold{0.5};     ///< |price_vs_poc| > X → аномалия
    double vp_confidence_adj{-0.1};

    // MACD Divergence
    double macd_histogram_extreme{0.0};         ///< Не используется напрямую (detect по тренду)
    double macd_confidence_adj{-0.1};

    // Bollinger Bands
    double bb_squeeze_bandwidth{0.02};          ///< BB bandwidth < X → сжатие
    double bb_breakout_percent_b_high{1.1};     ///< %B > X → пробой вверх
    double bb_breakout_percent_b_low{-0.1};     ///< %B < X → пробой вниз
    double bb_confidence_adj{-0.1};

    // ADX Trend Strength
    double adx_strong_trend{50.0};              ///< ADX > X → экстремальный тренд
    double adx_no_trend{15.0};                  ///< ADX < X → отсутствие тренда
    double adx_confidence_adj{-0.1};

    // Book Imbalance
    double book_imbalance_threshold{0.7};       ///< |imbalance| > X → перекос стакана
    double book_imbalance_confidence_adj{-0.15};

    // Time-of-Day
    double tod_low_alpha_threshold{-0.3};       ///< alpha_score < X → неблагоприятный час
    double tod_confidence_adj{-0.1};

    // CUSUM Regime Change
    double cusum_confidence_adj{-0.2};

    // Momentum Divergence
    double momentum_divergence_threshold{0.5};  ///< |mom5 - mom20| > X → расхождение
    double momentum_confidence_adj{-0.1};

    // Book Instability
    double book_instability_threshold{0.7};     ///< instability > X → нестабильный стакан
    double book_instability_confidence_adj{-0.15};
};

/// Конфигурация AI Advisory
struct AIAdvisoryConfig {
    bool enabled{false};                        ///< Включён ли AI Advisory
    int timeout_ms{2000};                       ///< Таймаут обработки (мс) — жёсткий deadline
    double max_confidence_adjustment{0.5};       ///< Макс. суммарная корректировка
    double veto_severity_threshold{0.8};        ///< Severity >= X → полное вето (no trade)
    double caution_severity_threshold{0.55};    ///< Severity >= X → caution (size × multiplier)
    int64_t cooldown_ms{5000};                  ///< Кулдаун между одинаковыми рекомендациями (мс)

    // --- Гистерезис ---
    int hysteresis_clear_ticks{3};             ///< Тиков ниже caution-порога для сброса в Clear

    // --- Ансамблевая эскалация ---
    int ensemble_escalation_count{3};          ///< Мин. детекторов для эскалации severity
    double ensemble_escalation_bonus{0.15};    ///< Бонус к max_severity при эскалации

    // --- Caution режим ---
    double caution_size_multiplier{0.5};       ///< Множитель размера при Caution (0.5 = -50%)

    // --- Взвешенная агрегация ---
    bool use_severity_weighted_adjustment{true}; ///< Severity-weighted confidence aggregation

    AIDetectorThresholds thresholds;            ///< Пороги детекторов
};

/// Статус AI-сервиса
struct AIServiceStatus {
    bool available{false};
    bool healthy{false};
    int requests_total{0};
    int requests_failed{0};
    int advisories_generated{0};
    Timestamp last_success{0};
};

} // namespace tb::ai
