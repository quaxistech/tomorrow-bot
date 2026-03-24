#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::regime {

/// Детализированные режимы рынка
enum class DetailedRegime {
    StrongUptrend,       ///< Сильный восходящий тренд
    WeakUptrend,         ///< Слабый восходящий тренд
    StrongDowntrend,     ///< Сильный нисходящий тренд
    WeakDowntrend,       ///< Слабый нисходящий тренд
    MeanReversion,       ///< Возврат к среднему
    VolatilityExpansion, ///< Расширение волатильности
    LowVolCompression,   ///< Сжатие при низкой волатильности
    LiquidityStress,     ///< Стресс ликвидности
    SpreadInstability,   ///< Нестабильность спреда
    AnomalyEvent,        ///< Аномальное событие
    ToxicFlow,           ///< Токсичный поток ордеров
    Chop,                ///< Рубка/шум
    Undefined            ///< Режим не определён
};

/// Рекомендация по активации стратегии для данного режима
struct RegimeStrategyHint {
    StrategyId strategy_id;
    bool should_enable{false};
    double weight_multiplier{1.0}; ///< Множитель веса стратегии [0,2]
    std::string reason;
};

/// Событие смены режима
struct RegimeTransition {
    DetailedRegime from;
    DetailedRegime to;
    double confidence{0.0};
    Timestamp occurred_at{Timestamp(0)};
};

/// Полный снимок результатов классификации режима
struct RegimeSnapshot {
    RegimeLabel label{RegimeLabel::Unclear};             ///< Упрощённая метка
    DetailedRegime detailed{DetailedRegime::Undefined};  ///< Детализированный режим
    double confidence{0.0};                               ///< Уверенность в классификации [0,1]
    double stability{0.0};                                ///< Стабильность режима [0,1]
    std::vector<RegimeStrategyHint> strategy_hints;       ///< Рекомендации для стратегий
    std::optional<RegimeTransition> last_transition;      ///< Последняя смена режима
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};
};

std::string to_string(DetailedRegime regime);
RegimeLabel to_simple_label(DetailedRegime regime);

} // namespace tb::regime
