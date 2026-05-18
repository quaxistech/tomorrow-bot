#pragma once
/**
 * @file uncertainty_types.hpp
 * @brief Типы модуля неопределённости для USDT-M futures scalp-бота
 *
 * Структуры данных для оценки, декомпозиции и управления
 * неопределённостью в торговом конвейере:
 *   — 9 измерений неопределённости (режим, сигнал, данные, исполнение,
 *     портфель, ML, корреляция, переходы, операционная)
 *   — ранжированные драйверы неопределённости
 *   — рекомендации по режиму исполнения и кулдауну
 *   — диагностику для observability
 */

#include "common/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tb::uncertainty {

// ─── Конфигурация ────────────────────────────────────────────────

/// Веса и пороги для агрегации неопределённости.
///
/// Научное обоснование весов по умолчанию:
///   - Regime (0.20): основной структурный фактор; ошибка классификации делает
///     остальные сигналы ненадёжными (Lopez de Prado, "Advances in Financial ML").
///   - Signal (0.15): конфликты индикаторов повышают вероятность ложных срабатываний.
///   - Data quality (0.15): market microstructure noise растёт нелинейно при ухудшении
///     качества данных (Hasbrouck, "Empirical Market Microstructure").
///   - Execution (0.15): для скальпинга на фьючерсах slippage и spread напрямую
///     определяют break-even; при ≥10 bps spread profit margin исчезает.
///   - Portfolio (0.10): concentration risk и drawdown — главные факторы ruin probability.
///   - ML (0.10): ensemble disagreement коррелирует с prediction error (Lakshminarayanan 2017).
///   - Correlation (0.05): regime де-корреляции — "хвостовой" риск, реже но сильнее.
///   - Transition (0.05): переходные периоды кратковременны, но с повышенным шумом.
///   - Operational (0.05): инфра-проблемы (feed staleness, API latency) — guard.
///
/// Scalping refactor 2026-05: 9-мерная композитная модель удалена.
/// Engine теперь использует max-of-priorities по 4 hard-сигналам
/// (spread, data freshness, VPIN, book_instability) и среднее по 3 soft-сигналам.
/// Веса per-dimension (w_*) больше не нужны.
struct UncertaintyConfig {
    // --- Пороги уровней ---
    double threshold_low{0.25};         ///< Граница Low → Moderate
    double threshold_moderate{0.50};    ///< Граница Moderate → High
    double threshold_high{0.75};        ///< Граница High → Extreme

    // --- Гистерезис (предотвращает осцилляцию между уровнями) ---
    double hysteresis_down{0.05};       ///< Запас для понижения уровня

    // --- EMA-сглаживание ---
    double ema_alpha{0.15};             ///< Коэффициент EMA для persistent_score [0,1]

    // --- Размер и порог ---
    double size_floor{0.10};            ///< Минимальный size_multiplier (никогда не 0)
    double threshold_ceiling{2.5};      ///< Максимальный threshold_multiplier

    // --- Cooldown ---
    int consecutive_extreme_for_cooldown{3}; ///< Число подряд Extreme для активации кулдауна
    int64_t cooldown_duration_ns{60'000'000'000LL}; ///< Длительность кулдауна (нс)
};

// ─── Измерения неопределённости ──────────────────────────────────

/// Размерности неопределённости
struct UncertaintyDimensions {
    double regime_uncertainty{0.5};       ///< Неопределённость классификации режима [0,1]
    double signal_uncertainty{0.5};       ///< Неопределённость торговых сигналов [0,1]
    double data_quality_uncertainty{0.0}; ///< Неопределённость качества данных [0,1]
    double execution_uncertainty{0.0};    ///< Неопределённость исполнения [0,1]
    double portfolio_uncertainty{0.0};    ///< Неопределённость портфеля [0,1]
    double ml_uncertainty{0.0};           ///< Неопределённость ML-моделей [0,1]
    double correlation_uncertainty{0.0};  ///< Нестабильность корреляционной структуры [0,1]
    double transition_uncertainty{0.0};   ///< Неопределённость перехода между режимами [0,1]
    double operational_uncertainty{0.0};  ///< Операционная неопределённость [0,1]
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
    UncertaintyLevel level{UncertaintyLevel::Moderate};    ///< Агрегированный уровень
    double aggregate_score{0.5};                            ///< [0=точно, 1=полная неопределённость]
    UncertaintyDimensions dimensions;
    UncertaintyAction recommended_action{UncertaintyAction::Normal};
    double size_multiplier{1.0};       ///< Множитель размера позиции [size_floor, 1]
    double threshold_multiplier{1.0};  ///< Множитель порога [1, threshold_ceiling]
    std::string explanation;
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};
    std::vector<UncertaintyDriver> top_drivers;                                         ///< Топ-3 драйвера неопределённости
    ExecutionModeRecommendation execution_mode{ExecutionModeRecommendation::Normal};     ///< Рекомендуемый режим исполнения
    CooldownRecommendation cooldown;                                                    ///< Рекомендация по кулдауну
    double persistent_score{0.5};          ///< EMA-сглаженный скор (устойчивый тренд)
    double spike_score{0.0};               ///< Транзиентный пиковый скор (моментальный всплеск)
};

// ─── Диагностика ─────────────────────────────────────────────────

/// Диагностика модуля неопределённости для observability
struct UncertaintyDiagnostics {
    uint64_t total_assessments{0};         ///< Общее количество вызовов assess()
    uint64_t veto_count{0};                ///< Количество выданных вето (NoTrade)
    uint64_t cooldown_activations{0};      ///< Количество активаций кулдауна
    double avg_aggregate_score{0.5};       ///< Средний aggregate_score за окно
    Timestamp last_assessment{Timestamp(0)};  ///< Время последней оценки
};

// ─── Сериализация перечислений ───────────────────────────────────

std::string to_string(UncertaintyAction action);
std::string to_string(ExecutionModeRecommendation mode);

} // namespace tb::uncertainty
