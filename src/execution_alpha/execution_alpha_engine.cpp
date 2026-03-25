#include "execution_alpha/execution_alpha_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::execution_alpha {

RuleBasedExecutionAlpha::RuleBasedExecutionAlpha(
    Config config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

ExecutionAlphaResult RuleBasedExecutionAlpha::evaluate(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features)
{
    ExecutionAlphaResult result;
    result.computed_at = clock_->now();

    // Рассчитать срочность
    result.urgency_score = compute_urgency(intent, features);

    // Определить стиль исполнения
    result.recommended_style = determine_style(intent, features);

    // Если стиль = NoExecution, отказаться от исполнения
    if (result.recommended_style == ExecutionStyle::NoExecution) {
        result.should_execute = false;
        result.rationale = "Условия слишком токсичны для исполнения";
        logger_->warn("ExecutionAlpha", "NoExecution: условия рынка неблагоприятны",
                       {{"symbol", intent.symbol.get()}});
        return result;
    }

    // Оценить качество исполнения
    result.quality = estimate_quality(result.recommended_style, features);

    // Рассчитать лимитную цену
    result.recommended_limit_price = compute_limit_price(
        result.recommended_style, intent, features);

    // Рассчитать план нарезки
    result.slice_plan = compute_slice_plan(intent, features);

    // Сформировать обоснование
    std::ostringstream oss;
    oss << "Стиль=" << to_string(result.recommended_style)
        << " срочность=" << result.urgency_score
        << " стоимость=" << result.quality.total_cost_bps << "бп"
        << " P(fill)=" << result.quality.fill_probability;
    result.rationale = oss.str();

    result.should_execute = true;

    if (metrics_) {
        metrics_->counter("execution_alpha_evaluations",
                          {{"style", to_string(result.recommended_style)}})->increment();
    }

    return result;
}

ExecutionStyle RuleBasedExecutionAlpha::determine_style(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features) const
{
    const double spread_bps = features.microstructure.spread_bps;
    const double adverse = estimate_adverse_selection(features);
    const double urgency = compute_urgency(intent, features);

    // Правило 1: Слишком широкий спред → отказ
    if (spread_bps > config_.max_spread_bps_any) {
        return ExecutionStyle::NoExecution;
    }

    // Правило 2: Высокий риск неблагоприятного отбора → отказ
    if (adverse > config_.adverse_selection_threshold) {
        return ExecutionStyle::NoExecution;
    }

    // Правило 3: Высокая срочность → агрессивно
    if (urgency > config_.urgency_aggressive_threshold) {
        return ExecutionStyle::Aggressive;
    }

    // Правило 4: Низкая срочность и узкий спред → пассивно
    if (urgency < config_.urgency_passive_threshold &&
        spread_bps < config_.max_spread_bps_passive) {
        return ExecutionStyle::Passive;
    }

    // По умолчанию → гибридный
    return ExecutionStyle::Hybrid;
}

double RuleBasedExecutionAlpha::compute_urgency(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features) const
{
    // Базовая срочность из интента
    double urgency = intent.urgency;

    // Модификатор: высокая волатильность увеличивает срочность
    if (features.technical.volatility_valid && features.technical.volatility_5 > 0.0) {
        // Нормализованная волатильность добавляет до 0.2 срочности
        double vol_factor = std::min(features.technical.volatility_5 * 2.0, 0.2);
        urgency += vol_factor;
    }

    // Модификатор: сильный моментум увеличивает срочность
    if (features.technical.momentum_valid) {
        double momentum_abs = std::abs(features.technical.momentum_5);
        double momentum_factor = std::min(momentum_abs * 0.5, 0.15);
        urgency += momentum_factor;
    }

    // Ограничить диапазоном [0, 1]
    return std::clamp(urgency, 0.0, 1.0);
}

ExecutionQualityEstimate RuleBasedExecutionAlpha::estimate_quality(
    ExecutionStyle style,
    const features::FeatureSnapshot& features) const
{
    ExecutionQualityEstimate quality;

    // Стоимость спреда
    quality.spread_cost_bps = features.microstructure.spread_bps;

    // Ожидаемое проскальзывание зависит от стиля
    switch (style) {
        case ExecutionStyle::Passive:
        case ExecutionStyle::PostOnly:
            // Пассивный — нет проскальзывания, но низкая вероятность заполнения
            quality.estimated_slippage_bps = 0.0;
            quality.fill_probability = 0.4;
            break;
        case ExecutionStyle::Aggressive:
            // Агрессивный — высокое проскальзывание, высокая вероятность
            quality.estimated_slippage_bps = features.execution_context.estimated_slippage_bps;
            quality.fill_probability = 0.95;
            break;
        case ExecutionStyle::Hybrid:
            // Гибридный — среднее проскальзывание
            quality.estimated_slippage_bps = features.execution_context.estimated_slippage_bps * 0.5;
            quality.fill_probability = 0.7;
            break;
        case ExecutionStyle::NoExecution:
            quality.fill_probability = 0.0;
            break;
    }

    // Риск неблагоприятного отбора
    quality.adverse_selection_risk = estimate_adverse_selection(features);

    // Полная стоимость: для пассивного — только часть спреда, для агрессивного — спред + проскальзывание
    if (style == ExecutionStyle::Passive || style == ExecutionStyle::PostOnly) {
        quality.total_cost_bps = quality.spread_cost_bps * 0.3; // Получаем ребейт
    } else {
        quality.total_cost_bps = quality.spread_cost_bps + quality.estimated_slippage_bps;
    }

    return quality;
}

double RuleBasedExecutionAlpha::estimate_adverse_selection(
    const features::FeatureSnapshot& features) const
{
    double score = 0.0;
    int factors = 0;

    // Агрессивный поток — токсичный рынок
    if (features.microstructure.trade_flow_valid) {
        score += std::clamp(features.microstructure.aggressive_flow, 0.0, 1.0);
        factors++;
    }

    // Нестабильность стакана
    if (features.microstructure.instability_valid) {
        score += std::clamp(features.microstructure.book_instability, 0.0, 1.0);
        factors++;
    }

    // Широкий спред как индикатор токсичности
    if (features.microstructure.spread_valid) {
        double spread_score = std::clamp(features.microstructure.spread_bps / 100.0, 0.0, 1.0);
        score += spread_score;
        factors++;
    }

    if (factors == 0) return 0.0;
    return score / static_cast<double>(factors);
}

std::optional<Price> RuleBasedExecutionAlpha::compute_limit_price(
    ExecutionStyle style,
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features) const
{
    // Для агрессивного стиля — нет лимитной цены (рыночный ордер)
    if (style == ExecutionStyle::Aggressive || style == ExecutionStyle::NoExecution) {
        return std::nullopt;
    }

    const double mid = features.mid_price.get();
    const double spread = features.microstructure.spread;

    if (mid <= 0.0 || spread <= 0.0) {
        return std::nullopt;
    }

    // Для пассивного стиля — ставим на best bid/ask (не внутрь спреда)
    // Buy: ставим чуть ниже best bid (вглубь стакана для maker)
    // Sell: ставим чуть выше best ask (вглубь стакана для maker)
    double offset = spread * 0.2; // 20% от спреда от best bid/ask наружу
    double limit;

    if (intent.side == Side::Buy) {
        // Покупка — на best bid или чуть ниже (maker)
        limit = mid - (spread / 2.0) - offset;
    } else {
        // Продажа — на best ask или чуть выше (maker)
        limit = mid + (spread / 2.0) + offset;
    }

    return Price(limit);
}

std::optional<SlicePlan> RuleBasedExecutionAlpha::compute_slice_plan(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features) const
{
    if (!features.microstructure.liquidity_valid) {
        return std::nullopt;
    }

    // Доступная глубина (используем сторону покупки/продажи)
    double available_depth = (intent.side == Side::Buy)
        ? features.microstructure.bid_depth_5_notional
        : features.microstructure.ask_depth_5_notional;

    if (available_depth <= 0.0) {
        return std::nullopt;
    }

    const double order_notional = intent.suggested_quantity.get() *
                                   features.mid_price.get();
    const double ratio = order_notional / available_depth;

    // Если ордер мал относительно глубины — не нужна нарезка
    if (ratio < config_.large_order_slice_threshold) {
        return std::nullopt;
    }

    SlicePlan plan;
    // Количество частей пропорционально превышению порога
    plan.num_slices = std::max(2, static_cast<int>(ratio / config_.large_order_slice_threshold));
    plan.num_slices = std::min(plan.num_slices, 10); // Макс 10 частей
    plan.slice_interval_ms = 200.0 * plan.num_slices; // Больше частей → больше интервал
    plan.first_slice_pct = 1.0 / plan.num_slices;
    plan.rationale = "Ордер = " + std::to_string(ratio * 100.0) +
                     "% от глубины, разбиение на " + std::to_string(plan.num_slices) + " частей";

    return plan;
}

} // namespace tb::execution_alpha
