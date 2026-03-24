#pragma once
#include "execution_alpha/execution_alpha_types.hpp"
#include "strategy/strategy_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>

namespace tb::execution_alpha {

/// Интерфейс движка исполнительной альфы
class IExecutionAlphaEngine {
public:
    virtual ~IExecutionAlphaEngine() = default;

    /// Оценить параметры исполнения для данного намерения
    virtual ExecutionAlphaResult evaluate(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& features) = 0;
};

/// Реализация на основе правил
class RuleBasedExecutionAlpha : public IExecutionAlphaEngine {
public:
    struct Config {
        double max_spread_bps_passive{15.0};      ///< Макс спред для пассивного исполнения
        double max_spread_bps_any{50.0};           ///< Макс спред для любого исполнения
        double adverse_selection_threshold{0.7};    ///< Порог токсичного потока
        double urgency_passive_threshold{0.5};      ///< Ниже → пассивно
        double urgency_aggressive_threshold{0.8};   ///< Выше → агрессивно
        double large_order_slice_threshold{0.1};    ///< % от depth → нарезка
    };

    RuleBasedExecutionAlpha(Config config,
                            std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock,
                            std::shared_ptr<metrics::IMetricsRegistry> metrics);

    ExecutionAlphaResult evaluate(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& features) override;

private:
    /// Определить стиль исполнения
    ExecutionStyle determine_style(const strategy::TradeIntent& intent,
                                   const features::FeatureSnapshot& features) const;

    /// Рассчитать срочность
    double compute_urgency(const strategy::TradeIntent& intent,
                          const features::FeatureSnapshot& features) const;

    /// Оценить качество исполнения
    ExecutionQualityEstimate estimate_quality(ExecutionStyle style,
                                              const features::FeatureSnapshot& features) const;

    /// Оценить риск неблагоприятного отбора
    double estimate_adverse_selection(const features::FeatureSnapshot& features) const;

    /// Рассчитать рекомендуемую лимитную цену
    std::optional<Price> compute_limit_price(ExecutionStyle style,
                                             const strategy::TradeIntent& intent,
                                             const features::FeatureSnapshot& features) const;

    /// Рассчитать план нарезки
    std::optional<SlicePlan> compute_slice_plan(const strategy::TradeIntent& intent,
                                                const features::FeatureSnapshot& features) const;

    Config config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
};

} // namespace tb::execution_alpha
