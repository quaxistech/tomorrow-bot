#pragma once
#include "execution_alpha/execution_alpha_types.hpp"
#include "strategy/strategy_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::execution_alpha {

/// Интерфейс движка исполнительной альфы
class IExecutionAlphaEngine {
public:
    virtual ~IExecutionAlphaEngine() = default;

    /// Оценить параметры исполнения для данного намерения
    virtual ExecutionAlphaResult evaluate(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& features,
        const uncertainty::UncertaintySnapshot& uncertainty) = 0;
};

/// Реализация на основе правил (production-grade)
class RuleBasedExecutionAlpha : public IExecutionAlphaEngine {
public:
    struct Config {
        // ── Базовые пороги ──
        // ИСПРАВЛЕНИЕ: Micro-cap altcoins имеют спреды 50-150bps.
        // Старые значения (15bps passive / 50bps any) блокировали микрокапы.
        double max_spread_bps_passive{60.0};     ///< Макс спред для пассивного исполнения (micro-cap реалистично)
        double max_spread_bps_any{150.0};        ///< Макс спред для любого исполнения (micro-cap worst-case)
        double adverse_selection_threshold{0.7};  ///< Порог токсичности для NoExecution
        double urgency_passive_threshold{0.5};    ///< Ниже → пассивно
        double urgency_aggressive_threshold{0.8}; ///< Выше → агрессивно
        double large_order_slice_threshold{0.1};  ///< % от depth → нарезка

        // ── VPIN интеграция ──
        double vpin_toxic_threshold{0.65};   ///< VPIN выше этого → высокая токсичность
        double vpin_weight{0.40};            ///< Вес VPIN в расчёте adverse selection

        // ── Дисбаланс стакана ──
        double imbalance_favorable_threshold{0.30};   ///< Имбаланс в нашу пользу → Passive предпочтителен
        double imbalance_unfavorable_threshold{0.30};  ///< Имбаланс против нас → предпочесть Hybrid

        // ── Расчёт лимитной цены ──
        bool   use_weighted_mid_price{true};    ///< Использовать взвешенную среднюю для ценообразования
        double limit_price_passive_bps{3.0};    ///< Улучшение к best bid/ask для maker [bps]

        // ── Модификаторы срочности ──
        double urgency_cusum_boost{0.15};   ///< Прирост срочности при сигнале CUSUM
        double urgency_tod_weight{0.10};    ///< Вес time-of-day alpha score в срочности

        // ── Вероятность заполнения ──
        double min_fill_probability_passive{0.25}; ///< Нижняя граница fill_prob для пассивного стиля

        // ── PostOnly условия (премиальный maker, нулевые комиссии) ──
        double postonly_spread_threshold_bps{4.5};  ///< Спред ниже этого → PostOnly кандидат
        double postonly_urgency_max{0.35};           ///< Срочность ниже этого → PostOnly кандидат
        double postonly_adverse_max{0.35};           ///< Токсичность ниже этого → PostOnly кандидат
    };

    RuleBasedExecutionAlpha(Config config,
                            std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock,
                            std::shared_ptr<metrics::IMetricsRegistry> metrics);

    ExecutionAlphaResult evaluate(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& features,
        const uncertainty::UncertaintySnapshot& uncertainty) override;

private:
    /// Проверить минимальное качество данных для принятия решений.
    /// При недостаточном качестве — graceful degradation.
    bool validate_features(const features::FeatureSnapshot& features) const;

    /// Определить стиль исполнения с учётом VPIN, дисбаланса стакана, срочности
    ExecutionStyle determine_style(const strategy::TradeIntent& intent,
                                   const features::FeatureSnapshot& features,
                                   double urgency,
                                   double adverse_score,
                                   double directional_imbalance) const;

    /// Рассчитать срочность с учётом CUSUM, time-of-day и моментума.
    /// Заполняет поля DecisionFactors для прозрачности.
    double compute_urgency(const strategy::TradeIntent& intent,
                           const features::FeatureSnapshot& features,
                           DecisionFactors& factors) const;

    /// Оценить вероятность заполнения на основе данных стакана (data-driven, не хардкод)
    double estimate_fill_probability(ExecutionStyle style,
                                     const strategy::TradeIntent& intent,
                                     const features::FeatureSnapshot& features,
                                     double directional_imbalance) const;

    /// Оценить качество исполнения
    ExecutionQualityEstimate estimate_quality(ExecutionStyle style,
                                              const strategy::TradeIntent& intent,
                                              const features::FeatureSnapshot& features,
                                              double adverse_score,
                                              double directional_imbalance) const;

    /// Оценить риск неблагоприятного отбора с VPIN и взвешенной агрегацией.
    /// Заполняет поля DecisionFactors.
    double estimate_adverse_selection(const features::FeatureSnapshot& features,
                                      DecisionFactors& factors) const;

    /// Направленный дисбаланс стакана: [-1=против нас, +1=в нашу пользу]
    double get_directional_imbalance(Side side,
                                     const features::FeatureSnapshot& features) const;

    /// Рассчитать рекомендуемую лимитную цену (weighted_mid + smart offset)
    std::optional<Price> compute_limit_price(ExecutionStyle style,
                                             const strategy::TradeIntent& intent,
                                             const features::FeatureSnapshot& features,
                                             bool& weighted_mid_used_out) const;

    /// Рассчитать план нарезки (интегрирует с TWAP executor)
    std::optional<SlicePlan> compute_slice_plan(const strategy::TradeIntent& intent,
                                                const features::FeatureSnapshot& features) const;

    Config config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
};

} // namespace tb::execution_alpha
