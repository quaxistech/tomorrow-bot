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

/// Одно условие, приведшее к классификации (для аудита и дебага)
struct ClassificationCondition {
    std::string indicator;    ///< Имя индикатора ("ADX", "RSI", "spread_bps", …)
    double value{0.0};        ///< Фактическое значение
    double threshold{0.0};    ///< Порог, с которым сравнивалось
    std::string op;           ///< Оператор (">", "<", ">=", "in_range", …)
    bool triggered{false};    ///< Сработало ли условие
};

/// Полное объяснение классификации: какие правила сработали, качество данных
struct ClassificationExplanation {
    DetailedRegime immediate_regime{DetailedRegime::Undefined};  ///< Мгновенный режим по правилам
    DetailedRegime persistent_regime{DetailedRegime::Undefined}; ///< Режим после hysteresis
    bool hysteresis_overrode{false};  ///< Hysteresis изменил итоговый режим

    std::vector<ClassificationCondition> triggered_conditions;   ///< Условия, которые сработали
    std::vector<ClassificationCondition> checked_conditions;     ///< Все проверенные условия

    int valid_indicator_count{0};
    int total_indicator_count{0};
    double data_quality_score{0.0};   ///< [0,1]

    int dwell_ticks{0};               ///< Сколько тиков в текущем режиме
    int confirmation_ticks_remaining{0}; ///< 0 = подтверждён

    std::string summary;              ///< Человекочитаемое резюме
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

    ClassificationExplanation explanation;                 ///< Объяснение классификации
};

std::string to_string(DetailedRegime regime);
RegimeLabel to_simple_label(DetailedRegime regime);

} // namespace tb::regime
