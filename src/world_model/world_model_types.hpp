#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace tb::world_model {

/// Расширенные состояния мира (более детализированные чем WorldStateLabel)
enum class WorldState {
    StableTrendContinuation,    ///< Устойчивое продолжение тренда
    FragileBreakout,            ///< Хрупкий пробой (может откатиться)
    CompressionBeforeExpansion, ///< Сжатие перед расширением
    ChopNoise,                  ///< Шум/боковик без выраженного направления
    ExhaustionSpike,            ///< Спайк истощения (потенциальный разворот)
    LiquidityVacuum,            ///< Вакуум ликвидности (опасно)
    ToxicMicrostructure,        ///< Токсичная микроструктура (манипуляции)
    PostShockStabilization,     ///< Стабилизация после шока
    Unknown                     ///< Состояние не определено
};

/// Метрика хрупкости состояния [0.0 = устойчиво, 1.0 = крайне хрупко]
struct FragilityScore {
    double value{0.5};
    bool valid{false};
};

/// Тенденция перехода состояния
enum class TransitionTendency {
    Stable,        ///< Тенденция оставаться в текущем состоянии
    Improving,     ///< Переход к более благоприятному состоянию
    Deteriorating, ///< Переход к менее благоприятному состоянию
    Ambiguous      ///< Направление перехода неясно
};

/// Пригодность текущего состояния для стратегии
struct StrategySuitability {
    StrategyId strategy_id;
    double suitability{0.0}; ///< [0.0 = непригоден, 1.0 = идеален]
    std::string reason;
};

/// Полный снимок мировой модели
struct WorldModelSnapshot {
    WorldState state{WorldState::Unknown};
    WorldStateLabel label{WorldStateLabel::Unknown};  ///< Упрощённая метка для обратной совместимости
    FragilityScore fragility;
    TransitionTendency tendency{TransitionTendency::Ambiguous};
    double persistence_score{0.5};       ///< Вероятность сохранения текущего состояния [0,1]
    std::vector<StrategySuitability> strategy_suitability;
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};

    /// Маппинг WorldState → WorldStateLabel для упрощённых потребителей
    static WorldStateLabel to_label(WorldState s);
};

/// Преобразование в строку для логирования
std::string to_string(WorldState state);
std::string to_string(TransitionTendency tendency);

} // namespace tb::world_model
