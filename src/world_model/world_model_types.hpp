#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace tb::world_model {

// ============================================================
// Константы
// ============================================================

/// Количество дискретных состояний мира
inline constexpr size_t kWorldStateCount = 9;

// ============================================================
// Перечисления
// ============================================================

/// Расширенные состояния мира (более детализированные чем WorldStateLabel)
enum class WorldState {
    StableTrendContinuation    = 0, ///< Устойчивое продолжение тренда
    FragileBreakout            = 1, ///< Хрупкий пробой (может откатиться)
    CompressionBeforeExpansion = 2, ///< Сжатие перед расширением
    ChopNoise                  = 3, ///< Шум/боковик без выраженного направления
    ExhaustionSpike            = 4, ///< Спайк истощения (потенциальный разворот)
    LiquidityVacuum            = 5, ///< Вакуум ликвидности (опасно)
    ToxicMicrostructure        = 6, ///< Токсичная микроструктура (манипуляции)
    PostShockStabilization     = 7, ///< Стабилизация после шока
    Unknown                    = 8  ///< Состояние не определено
};

/// Тенденция перехода состояния
enum class TransitionTendency {
    Stable,        ///< Тенденция оставаться в текущем состоянии
    Improving,     ///< Переход к более благоприятному состоянию
    Deteriorating, ///< Переход к менее благоприятному состоянию
    Ambiguous      ///< Направление перехода неясно
};

// ============================================================
// Структуры данных
// ============================================================

/// Метрика хрупкости состояния [0.0 = устойчиво, 1.0 = крайне хрупко]
struct FragilityScore {
    double value{0.5};
    bool valid{false};
    double confidence{0.5};  ///< Уверенность в оценке [0,1]
};

/// Пригодность текущего состояния для стратегии
struct StrategySuitability {
    StrategyId strategy_id{StrategyId("")};
    double suitability{0.0};           ///< Итоговый [0.0 = непригоден, 1.0 = идеален]
    double signal_suitability{0.0};    ///< Пригодность по сигналу
    double execution_suitability{1.0}; ///< Пригодность по исполнению
    double risk_suitability{1.0};      ///< Пригодность по риску
    bool vetoed{false};                ///< Жёсткий запрет
    std::string reason;
};

/// Одно проверенное условие классификации
struct ClassificationCondition {
    std::string rule_name;       ///< Имя правила (e.g. "ToxicMicrostructure")
    bool triggered{false};       ///< Сработало ли
    double proximity{0.0};       ///< Близость к срабатыванию [0=далеко, 1=на грани]
    std::string detail;          ///< Подробности ("book_instability=0.72 > 0.70")
};

/// Полное объяснение классификации (audit trail)
struct WorldModelExplanation {
    WorldState immediate_state{WorldState::Unknown};    ///< До гистерезиса
    WorldState confirmed_state{WorldState::Unknown};    ///< После гистерезиса
    bool hysteresis_overrode{false};                    ///< Гистерезис изменил результат

    std::vector<ClassificationCondition> triggered_conditions;  ///< Сработавшие правила
    std::vector<ClassificationCondition> checked_conditions;    ///< Все проверенные правила

    int valid_indicator_count{0};
    int total_indicator_count{0};
    double data_quality_score{0.0};         ///< [0,1]

    int dwell_ticks{0};                     ///< Тиков в текущем состоянии
    int confirmation_ticks_remaining{0};    ///< Осталось тиков до подтверждения

    /// Top-N факторов, определивших состояние
    std::vector<std::pair<std::string, double>> top_drivers;

    std::string summary;                    ///< Человекочитаемое резюме
};

/// Вероятностное распределение по состояниям
struct StateProbabilities {
    double values[kWorldStateCount]{};  ///< P(state_i) для каждого из 9 состояний
    bool valid{false};

    /// Вероятность конкретного состояния
    [[nodiscard]] double probability(WorldState s) const {
        return values[static_cast<int>(s)];
    }
};

/// Контекст перехода между состояниями
struct TransitionContext {
    TransitionTendency tendency{TransitionTendency::Ambiguous};
    double velocity{0.0};       ///< Скорость изменения quality score [-1,+1]
    double pressure{0.0};       ///< Давление перехода: чем выше, тем ближе смена [0,1]
    int transitions_recent{0};  ///< Переходы за последние N тиков
    WorldState previous_state{WorldState::Unknown};
};

/// Полный снимок мировой модели (v2 — обратно совместим с v1)
struct WorldModelSnapshot {
    // ── v1 поля (обратная совместимость) ─────────────────────
    WorldState state{WorldState::Unknown};
    WorldStateLabel label{WorldStateLabel::Unknown};
    FragilityScore fragility;
    TransitionTendency tendency{TransitionTendency::Ambiguous};
    double persistence_score{0.5};
    std::vector<StrategySuitability> strategy_suitability;
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};

    // ── v2 расширения ────────────────────────────────────────
    double confidence{0.5};                          ///< Общая уверенность в классификации [0,1]
    StateProbabilities state_probabilities;           ///< Вероятностное распределение
    TransitionContext transition;                     ///< Контекст перехода
    WorldModelExplanation explanation;                ///< Полный audit trail
    std::string model_version;                        ///< Версия модели для governance
    int dwell_ticks{0};                              ///< Тиков в текущем состоянии

    /// Маппинг WorldState → WorldStateLabel для упрощённых потребителей
    static WorldStateLabel to_label(WorldState s);
};

// ============================================================
// Обратная связь (feedback)
// ============================================================

/// Запись об исходе торговли в конкретном состоянии мира
struct WorldStateFeedback {
    WorldState state{WorldState::Unknown};
    StrategyId strategy_id{StrategyId("")};
    double pnl_bps{0.0};
    double slippage_bps{0.0};
    double max_adverse_excursion_bps{0.0};
    bool was_profitable{false};
    Timestamp timestamp{Timestamp(0)};
};

/// Накопленная статистика для пары (state, strategy)
struct StatePerformanceStats {
    int total_trades{0};
    int winning_trades{0};
    double total_pnl_bps{0.0};
    double avg_slippage_bps{0.0};
    double max_drawdown_bps{0.0};
    double ema_win_rate{0.5};      ///< EMA win-rate
    double ema_expectancy{0.0};    ///< EMA P&L per trade

    [[nodiscard]] double win_rate() const {
        return total_trades > 0 ? static_cast<double>(winning_trades) / total_trades : 0.5;
    }
};

// ============================================================
// Преобразование в строку
// ============================================================

std::string to_string(WorldState state);
std::string to_string(TransitionTendency tendency);

/// Индекс состояния для матриц и массивов.
/// BUG-S4-25 fix: clamp corrupted enum values to Unknown (8) to prevent
/// out-of-bounds array access in state_total_count / state_stay_count.
inline int state_index(WorldState s) {
    const int idx = static_cast<int>(s);
    return (idx >= 0 && idx <= static_cast<int>(WorldState::Unknown))
        ? idx
        : static_cast<int>(WorldState::Unknown);
}

} // namespace tb::world_model
