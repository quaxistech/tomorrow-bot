#include "execution/planner/execution_planner.hpp"
#include "uncertainty/uncertainty_types.hpp"

namespace tb::execution {

ExecutionPlanner::ExecutionPlanner(const ExecutionConfig& config,
                                   std::shared_ptr<logging::ILogger> logger)
    : config_(config)
    , logger_(std::move(logger))
{
    // BUG-S5-06 fix: logger_ is used unconditionally; nullptr crashes at first log call.
    if (!logger_) throw std::invalid_argument("ExecutionPlanner: logger must not be null");
}

ExecutionPlan ExecutionPlanner::plan(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const MarketExecutionContext& market,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    ExecutionPlan plan;

    // §13: Классифицировать действие
    plan.action = classify_action(intent);

    // §13: Установить кол-во из risk decision
    plan.planned_quantity = risk.approved_quantity;

    // §13: Выбрать стиль исполнения
    plan.style = choose_style(intent, exec_alpha, market, plan.action);

    // §13: Преобразовать стиль в параметры ордера
    plan.order_type = style_to_order_type(plan.style);
    plan.tif = style_to_tif(plan.style);

    // §13: Рассчитать цену
    if (plan.order_type == OrderType::Market) {
        // Market: используем mid_price/reference для оценки
        if (exec_alpha.recommended_limit_price) {
            plan.planned_price = *exec_alpha.recommended_limit_price;
        } else if (intent.snapshot_mid_price) {
            plan.planned_price = *intent.snapshot_mid_price;
        } else if (market.best_bid.get() > 0 && market.best_ask.get() > 0) {
            plan.planned_price = Price((market.best_bid.get() + market.best_ask.get()) / 2.0);
        }
    } else {
        plan.planned_price = compute_limit_price(intent, exec_alpha, market);
        // BUG-S29-07: if limit price couldn't be determined, fall back to market
        // rather than submitting a limit order with price=0 to the exchange.
        if (plan.planned_price.get() <= 0.0) {
            plan.order_type = OrderType::Market;
            plan.tif = TimeInForce::ImmediateOrCancel;
            plan.style = PlannedExecutionStyle::AggressiveMarket;
            plan.reasons.push_back("limit_price_unavailable→fallback_market");
        }
    }

    // §13: Таймаут
    plan.timeout_ms = compute_timeout(plan.action, plan.style);

    // §13: Fallback
    if (plan.style == PlannedExecutionStyle::SmartFallback) {
        plan.enable_fallback = true;
        plan.fallback_after_ms = config_.limit_fallback_to_market_ms;
    }

    // §19: Reduce-only
    plan.reduce_only = should_reduce_only(intent, plan.action);

    // §6.4: Explainability — записать причины
    plan.reasons.push_back("action=" + to_string(plan.action));
    plan.reasons.push_back("spread=" + std::to_string(market.spread_bps) + "bps");
    plan.reasons.push_back("urgency=" + std::to_string(exec_alpha.urgency_score));

    if (plan.reduce_only) {
        plan.reasons.push_back("reduce_only=true");
    }

    if (uncertainty.level == UncertaintyLevel::High) {
        plan.reasons.push_back("uncertainty=high→aggressive");
    }

    // §14: Для скальпинга — если спред сильно высокий, форсируем CancelIfNotFilled
    if (market.spread_bps > config_.spread_bps_aggressive_threshold &&
        plan.action == ExecutionAction::OpenPosition &&
        plan.style != PlannedExecutionStyle::AggressiveMarket) {
        plan.style = PlannedExecutionStyle::CancelIfNotFilled;
        plan.order_type = style_to_order_type(plan.style);
        plan.tif = style_to_tif(plan.style);
        plan.timeout_ms = std::min(plan.timeout_ms, static_cast<int64_t>(5000));
        plan.reasons.push_back("spread_too_wide→cancel_if_not_filled");
    }

    // §14: Устаревший маркет контекст → прямой market (самое безопасное)
    if (market.is_stale) {
        plan.style = PlannedExecutionStyle::AggressiveMarket;
        plan.order_type = OrderType::Market;
        plan.tif = TimeInForce::ImmediateOrCancel;
        plan.reasons.push_back("stale_data→forced_market");
    }

    logger_->debug("ExecutionPlanner", "Составлен план исполнения",
        {{"symbol", intent.symbol.get()},
         {"action", to_string(plan.action)},
         {"style", to_string(plan.style)},
         {"order_type", plan.order_type == OrderType::Market ? "market" : "limit"},
         {"timeout_ms", std::to_string(plan.timeout_ms)},
         {"reduce_only", plan.reduce_only ? "true" : "false"}});

    return plan;
}

ExecutionAction ExecutionPlanner::classify_action(const strategy::TradeIntent& intent) const {
    using SI = strategy::SignalIntent;
    using ER = strategy::ExitReason;

    // Emergency exits
    if (intent.exit_reason == ER::EmergencyExit) {
        return ExecutionAction::EmergencyFlattenSymbol;
    }

    // Classify by signal intent
    switch (intent.signal_intent) {
        case SI::LongEntry:
        case SI::ShortEntry:
            return ExecutionAction::OpenPosition;

        case SI::LongExit:
        case SI::ShortExit:
            return ExecutionAction::ClosePosition;

        case SI::ReducePosition:
            return ExecutionAction::ReducePosition;

        case SI::Hold:
            return ExecutionAction::NoAction;
    }

    // Fallback: по trade_side
    if (intent.trade_side == TradeSide::Close) {
        return ExecutionAction::ClosePosition;
    }
    return ExecutionAction::OpenPosition;
}

PlannedExecutionStyle ExecutionPlanner::choose_style(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const MarketExecutionContext& market,
    ExecutionAction action) const
{
    // §19: Exits / reduce / emergency → всегда aggressive market
    if (action == ExecutionAction::ClosePosition ||
        action == ExecutionAction::ReducePosition ||
        action == ExecutionAction::EmergencyFlattenSymbol ||
        action == ExecutionAction::EmergencyFlattenAll) {
        return PlannedExecutionStyle::ReduceOnly;
    }

    // §14: Учесть рекомендацию execution alpha
    using ES = execution_alpha::ExecutionStyle;
    switch (exec_alpha.recommended_style) {
        case ES::Aggressive:
            return PlannedExecutionStyle::AggressiveMarket;
        case ES::PostOnly:
            // Post-only alpha signal maps to passive limit planning.
            return PlannedExecutionStyle::PassiveLimit;
        case ES::NoExecution:
            // Не должен дойти сюда (отфильтровано выше), но на всякий случай
            return PlannedExecutionStyle::AggressiveMarket;
        case ES::Passive:
        case ES::Hybrid:
            break;  // Дальше решаем по спреду/urgency
    }

    // §14: Решение по спреду и urgency
    double urgency = exec_alpha.urgency_score;
    double spread = market.spread_bps;

    if (urgency >= config_.urgency_aggressive_threshold) {
        return PlannedExecutionStyle::AggressiveMarket;
    }

    if (spread < config_.spread_bps_passive_threshold && urgency < config_.urgency_passive_threshold) {
        return PlannedExecutionStyle::PassiveLimit;
    }

    // §14: Smart fallback — начать с limit, при таймауте → market
    if (config_.enable_market_fallback) {
        return PlannedExecutionStyle::SmartFallback;
    }

    return PlannedExecutionStyle::AggressiveMarket;
}

OrderType ExecutionPlanner::style_to_order_type(PlannedExecutionStyle style) const {
    switch (style) {
        case PlannedExecutionStyle::AggressiveMarket:
            return OrderType::Market;
        case PlannedExecutionStyle::PassiveLimit:
        case PlannedExecutionStyle::SmartFallback:
        case PlannedExecutionStyle::CancelIfNotFilled:
            return OrderType::Limit;
        case PlannedExecutionStyle::ReduceOnly:
            return OrderType::Market;
        case PlannedExecutionStyle::PostOnlyLimit:
            return OrderType::PostOnly;
    }
    return OrderType::Market;
}

TimeInForce ExecutionPlanner::style_to_tif(PlannedExecutionStyle style) const {
    switch (style) {
        case PlannedExecutionStyle::AggressiveMarket:
        case PlannedExecutionStyle::ReduceOnly:
            return TimeInForce::ImmediateOrCancel;
        case PlannedExecutionStyle::PassiveLimit:
        case PlannedExecutionStyle::SmartFallback:
            return TimeInForce::GoodTillCancel;
        case PlannedExecutionStyle::PostOnlyLimit:
            return TimeInForce::GoodTillCancel;
        case PlannedExecutionStyle::CancelIfNotFilled:
            return TimeInForce::ImmediateOrCancel;
    }
    return TimeInForce::ImmediateOrCancel;
}

Price ExecutionPlanner::compute_limit_price(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const MarketExecutionContext& market) const
{
    // Приоритет: exec_alpha > intent > market mid
    if (exec_alpha.recommended_limit_price) {
        return *exec_alpha.recommended_limit_price;
    }

    if (intent.limit_price) {
        return *intent.limit_price;
    }

    // §14: Price improvement от market mid
    if (market.best_bid.get() > 0 && market.best_ask.get() > 0) {
        double mid = (market.best_bid.get() + market.best_ask.get()) / 2.0;
        double improvement = mid * config_.limit_price_improvement_bps / 10000.0;
        if (intent.side == Side::Buy) {
            return Price(mid - improvement);  // Покупаем чуть дешевле mid
        } else {
            return Price(mid + improvement);  // Продаём чуть дороже mid
        }
    }

    if (intent.snapshot_mid_price) {
        return *intent.snapshot_mid_price;
    }

    return Price(0.0);
}

int64_t ExecutionPlanner::compute_timeout(ExecutionAction action,
                                           PlannedExecutionStyle style) const {
    // §30: Exits → exit_timeout, entries → entry_timeout
    switch (action) {
        case ExecutionAction::ClosePosition:
        case ExecutionAction::ReducePosition:
        case ExecutionAction::EmergencyFlattenSymbol:
        case ExecutionAction::EmergencyFlattenAll:
            return config_.exit_timeout_ms;
        default:
            if (style == PlannedExecutionStyle::PassiveLimit) {
                // Passive entries can wait longer
                return config_.entry_timeout_ms;
            }
            return config_.entry_timeout_ms;
    }
}

bool ExecutionPlanner::should_reduce_only(const strategy::TradeIntent& intent,
                                           ExecutionAction action) const {
    if (action == ExecutionAction::ClosePosition ||
        action == ExecutionAction::ReducePosition ||
        action == ExecutionAction::EmergencyFlattenSymbol ||
        action == ExecutionAction::EmergencyFlattenAll) {
        return true;
    }

    // Close trade_side → reduce only
    if (intent.trade_side == TradeSide::Close) {
        return true;
    }

    return false;
}

} // namespace tb::execution
