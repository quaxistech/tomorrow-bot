#pragma once
/**
 * @file uncertainty_types.hpp
 * @brief Типы модуля неопределённости (v2)
 *
 * Определяет все структуры данных для оценки, декомпозиции и управления
 * неопределённостью в торговом конвейере. Расширенная версия включает:
 *   — дополнительные измерения (ML, корреляция, переходы, операционная)
 *   — ранжированные драйверы неопределённости
 *   — рекомендации по режиму исполнения и кулдауну
 *   — обратную связь после сделки (калибровка)
 *   — диагностику для observability
 *
 * Обратная совместимость с v1 полностью сохранена.
 */

#include "common/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tb::uncertainty {

// ─── Конфигурация ────────────────────────────────────────────────

/// Веса и пороги для агрегации неопределённости
struct UncertaintyConfig {
    // --- Веса измерений (должны суммироваться в 1.0) ---
    double w_regime{0.25};              ///< Вес режимной неопределённости
    double w_signal{0.20};              ///< Вес сигнальной неопределённости
    double w_data_quality{0.10};        ///< Вес качества данных
    double w_execution{0.10};           ///< Вес неопределённости исполнения
    double w_portfolio{0.10};           ///< Вес портфельной неопределённости
    double w_ml{0.10};                  ///< Вес ML-неопределённости
    double w_correlation{0.05};         ///< Вес корреляционной неопределённости
    double w_transition{0.05};          ///< Вес переходной неопределённости
    double w_operational{0.05};         ///< Вес операционной неопределённости

    // --- Пороги уровней ---
    double threshold_low{0.25};         ///< Граница Low → Moderate
    double threshold_moderate{0.50};    ///< Граница Moderate → High
    double threshold_high{0.75};        ///< Граница High → Extreme

    // --- Гистерезис (предотвращает осцилляцию между уровнями) ---
    double hysteresis_up{0.03};         ///< Дополнительный запас для повышения уровня
    double hysteresis_down{0.05};       ///< Дополнительный запас для понижения уровня

    // --- EMA-сглаживание ---
    double ema_alpha{0.15};             ///< Коэффициент EMA для persistent_score [0,1]

    // --- Размер и порог ---
    double size_floor{0.10};            ///< Минимальный size_multiplier (никогда не 0)
    double threshold_ceiling{3.0};      ///< Максимальный threshold_multiplier

    // --- Калибровка ---
    double calibration_decay{0.995};    ///< Экспоненциальный распад уверенности калибровки
    uint32_t min_feedback_samples{50};  ///< Минимум выборок для калибровки
};

// ─── Измерения неопределённости ──────────────────────────────────

/// Размерности неопределённости
struct UncertaintyDimensions {
    // --- v1 (обратная совместимость) ---
    double regime_uncertainty{0.5};       ///< Неопределённость классификации режима [0,1]
    double signal_uncertainty{0.5};       ///< Неопределённость торговых сигналов [0,1]
    double data_quality_uncertainty{0.0}; ///< Неопределённость качества данных [0,1]
    double execution_uncertainty{0.0};    ///< Неопределённость исполнения [0,1]
    double portfolio_uncertainty{0.0};    ///< Неопределённость портфеля [0,1] — Фаза 4

    // --- v2 ---
    double ml_uncertainty{0.0};           ///< Неопределённость ML-моделей (разброс ансамбля) [0,1]
    double correlation_uncertainty{0.0};  ///< Нестабильность корреляционной структуры [0,1]
    double transition_uncertainty{0.0};   ///< Неопределённость перехода между режимами [0,1]
    double operational_uncertainty{0.0};  ///< Операционная неопределённость (API, задержки) [0,1]
};

// ─── Перечисления ────────────────────────────────────────────────

/// Рекомендация по действию на основе неопределённости
enum class UncertaintyAction {
    Normal,          ///< Нормальная торговля
    ReducedSize,     ///< Уменьшить размер позиции
    HigherThreshold, ///< Повысить порог для входа в позицию
    NoTrade          ///< Запрет на новые сделки
};

/// Рекомендация по режиму исполнения
enum class ExecutionModeRecommendation {
    Normal,          ///< Стандартный режим — все стратегии активны
    Conservative,    ///< Консервативный — уменьшенные размеры, повышенные пороги
    DefensiveOnly,   ///< Только защитные стратегии (хедж, стоп-лосс)
    HaltNewEntries   ///< Запрет новых входов — только управление существующими позициями
};

// ─── Драйверы и декомпозиция ─────────────────────────────────────

/// Ранжированный драйвер неопределённости (для объяснения решений)
struct UncertaintyDriver {
    std::string dimension;       ///< Имя измерения (например, "regime_uncertainty")
    double contribution{0.0};    ///< Взвешенный вклад в aggregate_score [0,1]
    double raw_value{0.0};       ///< Исходное значение измерения [0,1]
    std::string description;     ///< Человекочитаемое объяснение на русском
};

// ─── Кулдаун ─────────────────────────────────────────────────────

/// Рекомендация по кулдауну после всплеска неопределённости
struct CooldownRecommendation {
    bool active{false};                ///< Кулдаун активен?
    int64_t remaining_ns{0};           ///< Оставшееся время кулдауна (наносекунды)
    double decay_factor{1.0};          ///< Множитель затухания [0,1] — 1.0 = нет эффекта
    std::string trigger_reason;        ///< Причина активации кулдауна
};

// ─── Снимок неопределённости ─────────────────────────────────────

/// Полный результат оценки неопределённости
struct UncertaintySnapshot {
    // --- v1 (обратная совместимость) ---
    UncertaintyLevel level{UncertaintyLevel::Moderate};    ///< Агрегированный уровень
    double aggregate_score{0.5};                            ///< [0=точно, 1=полная неопределённость]
    UncertaintyDimensions dimensions;
    UncertaintyAction recommended_action{UncertaintyAction::Normal};
    double size_multiplier{1.0};       ///< Множитель размера позиции [0,1]
    double threshold_multiplier{1.0};  ///< Множитель порога [1, +inf)
    std::string explanation;
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};

    // --- v2 ---
    std::vector<UncertaintyDriver> top_drivers;                                         ///< Топ-3 драйвера неопределённости
    ExecutionModeRecommendation execution_mode{ExecutionModeRecommendation::Normal};     ///< Рекомендуемый режим исполнения
    CooldownRecommendation cooldown;                                                    ///< Рекомендация по кулдауну
    int64_t freshness_ns{0};               ///< Возраст снимка (наносекунды с момента вычисления)
    double calibration_confidence{0.5};    ///< Уверенность в калибровке модели [0,1]
    uint32_t model_version{1};             ///< Версия модели (champion-challenger)
    double persistent_score{0.5};          ///< EMA-сглаженный скор (устойчивый тренд)
    double spike_score{0.0};               ///< Транзиентный пиковый скор (моментальный всплеск)
};

// ─── Обратная связь ──────────────────────────────────────────────

/// Обратная связь после сделки для калибровки модели неопределённости
struct UncertaintyFeedback {
    Symbol symbol{Symbol("")};                    ///< Инструмент
    Timestamp trade_time{Timestamp(0)};           ///< Время сделки
    double predicted_uncertainty{0.5};            ///< Предсказанная неопределённость на момент сделки
    double realized_slippage{0.0};                ///< Реализованное проскальзывание (абс. %)
    double realized_pnl{0.0};                     ///< Реализованный P&L сделки
    double realized_volatility{0.0};              ///< Реализованная волатильность за период
    bool was_stopped_out{false};                  ///< Был ли стоп-лосс
    UncertaintyAction action_taken{UncertaintyAction::Normal};  ///< Действие, принятое системой
    std::string notes;                            ///< Дополнительные пометки
};

// ─── Диагностика ─────────────────────────────────────────────────

/// Диагностика модуля неопределённости для observability
struct UncertaintyDiagnostics {
    uint64_t total_assessments{0};         ///< Общее количество вызовов assess()
    uint64_t veto_count{0};                ///< Количество выданных вето (NoTrade)
    uint64_t cooldown_activations{0};      ///< Количество активаций кулдауна
    double avg_assessment_latency_us{0.0}; ///< Средняя задержка assess() (микросекунды)
    double max_assessment_latency_us{0.0}; ///< Максимальная задержка assess() (микросекунды)
    double avg_aggregate_score{0.5};       ///< Средний aggregate_score за окно
    double calibration_error{0.0};         ///< Ошибка калибровки (Brier score) [0,1]
    uint32_t feedback_samples{0};          ///< Количество полученных сэмплов обратной связи
    uint32_t active_model_version{1};      ///< Текущая активная версия модели
    Timestamp last_assessment{Timestamp(0)};  ///< Время последней оценки
    Timestamp last_feedback{Timestamp(0)};    ///< Время последней обратной связи
};

// ─── Сериализация перечислений ───────────────────────────────────

std::string to_string(UncertaintyAction action);
std::string to_string(ExecutionModeRecommendation mode);

} // namespace tb::uncertainty
