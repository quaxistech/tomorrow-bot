#pragma once
#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::strategy {

/// Тип торгового намерения для спотовой торговли.
/// На споте короткие позиции невозможны — SELL означает выход из лонга.
enum class SignalIntent {
    LongEntry,          ///< Открытие длинной позиции (BUY)
    LongExit,           ///< Полное закрытие длинной позиции (SELL)
    ReducePosition,     ///< Частичное сокращение позиции
    Hold                ///< Удерживать текущую позицию без действий
};

/// Причина выхода из позиции (для аналитики и attribution)
enum class ExitReason {
    None,               ///< Не выход (entry или hold)
    TakeProfit,         ///< Целевая прибыль достигнута
    StopLoss,           ///< Стоп-лосс сработал
    TrailingStop,       ///< Trailing stop сработал
    TrendFailure,       ///< Тренд развернулся / ослаб
    RangeTopExit,       ///< Цена у верхней границы рейнджа (mean reversion exit)
    VolatilitySpikeExit,///< Аномальный всплеск волатильности
    InventoryRiskExit,  ///< Управление инвентарем (слишком долгое удержание)
    SignalDecay,        ///< Сигнал деградировал, alpha исчезла
    TimeExit,           ///< Превышено максимальное время удержания
    RegimeChange        ///< Смена рыночного режима
};

inline const char* to_string(SignalIntent si) {
    switch (si) {
        case SignalIntent::LongEntry:       return "LongEntry";
        case SignalIntent::LongExit:        return "LongExit";
        case SignalIntent::ReducePosition:  return "ReducePosition";
        case SignalIntent::Hold:            return "Hold";
    }
    return "Unknown";
}

inline const char* to_string(ExitReason er) {
    switch (er) {
        case ExitReason::None:               return "None";
        case ExitReason::TakeProfit:         return "TakeProfit";
        case ExitReason::StopLoss:           return "StopLoss";
        case ExitReason::TrailingStop:       return "TrailingStop";
        case ExitReason::TrendFailure:       return "TrendFailure";
        case ExitReason::RangeTopExit:       return "RangeTopExit";
        case ExitReason::VolatilitySpikeExit:return "VolatilitySpikeExit";
        case ExitReason::InventoryRiskExit:  return "InventoryRiskExit";
        case ExitReason::SignalDecay:        return "SignalDecay";
        case ExitReason::TimeExit:           return "TimeExit";
        case ExitReason::RegimeChange:       return "RegimeChange";
    }
    return "Unknown";
}

/// Торговое намерение — предложение стратегии (НЕ ордер!)
struct TradeIntent {
    StrategyId strategy_id{StrategyId("")};
    StrategyVersion strategy_version{StrategyVersion(0)};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    SignalIntent signal_intent{SignalIntent::LongEntry}; ///< Спотовая семантика сигнала
    ExitReason exit_reason{ExitReason::None};            ///< Причина выхода (для SELL-сигналов)
    Quantity suggested_quantity{Quantity(0.0)};
    std::optional<Price> limit_price;                 ///< Рекомендуемая лимитная цена (или nullopt для рыночного)
    double conviction{0.0};                           ///< Убеждённость стратегии [0.0, 1.0]
    std::string signal_name;                          ///< Имя сигнала ("ma_crossover", "rsi_oversold", ...)
    std::vector<std::string> reason_codes;            ///< Коды причин
    Timestamp generated_at{Timestamp(0)};
    CorrelationId correlation_id{CorrelationId("")};

    double entry_score{0.0};  ///< Оценка точки входа [0,1]
    double urgency{0.0};      ///< Срочность [0=не срочно, 1=очень срочно]
};

/// Контекст стратегии — входные данные для оценки
struct StrategyContext {
    features::FeatureSnapshot features;
    RegimeLabel regime{RegimeLabel::Unclear};
    WorldStateLabel world_state{WorldStateLabel::Unknown};
    UncertaintyLevel uncertainty{UncertaintyLevel::Moderate};
    double uncertainty_size_multiplier{1.0};
    double uncertainty_threshold_multiplier{1.0};
    bool is_strategy_enabled{true};
    double strategy_weight{1.0};
};

/// Метаданные стратегии
struct StrategyMeta {
    StrategyId id{StrategyId("")};
    StrategyVersion version{StrategyVersion(0)};
    std::string name;
    std::string description;
    std::vector<RegimeLabel> preferred_regimes;    ///< Режимы, в которых стратегия эффективна
    std::vector<std::string> required_features;    ///< Требуемые признаки
};

} // namespace tb::strategy
