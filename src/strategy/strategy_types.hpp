#pragma once
#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::strategy {

/// Торговое намерение — предложение стратегии (НЕ ордер!)
struct TradeIntent {
    StrategyId strategy_id{StrategyId("")};
    StrategyVersion strategy_version{StrategyVersion(0)};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
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
