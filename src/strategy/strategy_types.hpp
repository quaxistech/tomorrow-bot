#pragma once
#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::strategy {

/// Тип торгового намерения (spot + futures).
/// На фьючерсах поддерживаются SHORT позиции через ShortEntry/ShortExit.
enum class SignalIntent {
    LongEntry,          ///< Открытие длинной позиции (BUY)
    LongExit,           ///< Полное закрытие длинной позиции (SELL)
    ShortEntry,         ///< Открытие короткой позиции (SELL на фьючерсах)
    ShortExit,          ///< Закрытие короткой позиции (BUY на фьючерсах)
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
    RegimeChange,       ///< Смена рыночного режима
    PartialReduction    ///< Частичное сокращение позиции (SignalIntent::ReducePosition)
};

inline const char* to_string(SignalIntent si) {
    switch (si) {
        case SignalIntent::LongEntry:       return "LongEntry";
        case SignalIntent::LongExit:        return "LongExit";
        case SignalIntent::ShortEntry:      return "ShortEntry";
        case SignalIntent::ShortExit:       return "ShortExit";
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
        case ExitReason::PartialReduction:   return "PartialReduction";
    }
    return "Unknown";
}

/// Торговое намерение — предложение стратегии (НЕ ордер!)
struct TradeIntent {
    StrategyId strategy_id{StrategyId("")};
    StrategyVersion strategy_version{StrategyVersion(0)};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};     ///< Сторона позиции (Long/Short)
    SignalIntent signal_intent{SignalIntent::LongEntry}; ///< Семантика сигнала
    TradeSide trade_side{TradeSide::Open};               ///< Открытие/закрытие (для фьючерсов)
    ExitReason exit_reason{ExitReason::None};            ///< Причина выхода (для SELL-сигналов)
    Quantity suggested_quantity{Quantity(0.0)};
    std::optional<Price> limit_price;                 ///< Рекомендуемая лимитная цена (или nullopt для рыночного)
    std::optional<Price> snapshot_mid_price;          ///< Mid price из snapshot на момент генерации (fallback для market ордеров)
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
    bool futures_enabled{false};  ///< Фьючерсы включены → стратегии могут генерировать ShortEntry/ShortExit
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
