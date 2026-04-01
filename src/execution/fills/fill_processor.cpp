#include "execution/fills/fill_processor.hpp"
#include "common/constants.hpp"

namespace tb::execution {

using tb::common::fees::kDefaultTakerFeePct;

FillProcessor::FillProcessor(
    OrderRegistry& registry,
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    const ExecutionConfig& config)
    : registry_(registry)
    , portfolio_(std::move(portfolio))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , config_(config)
{
}

bool FillProcessor::process_market_fill(
    const OrderId& order_id, Quantity filled_qty, Price fill_price,
    const OrderId& exchange_order_id)
{
    // §22: Idempotency guard
    if (registry_.is_fill_applied(order_id)) {
        logger_->warn("FillProcessor", "Fill уже применён (idempotency guard)",
            {{"order_id", order_id.get()}});
        return false;
    }

    auto opt = registry_.get_order(order_id);
    if (!opt) {
        logger_->error("FillProcessor", "Ордер не найден для fill",
            {{"order_id", order_id.get()}});
        return false;
    }

    auto order = *opt;

    // FSM: PendingAck → Filled
    if (!registry_.transition(order_id, OrderState::Filled, "Market order filled")) {
        logger_->error("FillProcessor", "FSM-переход в Filled не удался",
            {{"order_id", order_id.get()}});
        return false;
    }

    // Обновить запись ордера
    order.state = OrderState::Filled;
    order.exchange_order_id = exchange_order_id;
    order.filled_quantity = filled_qty;
    order.remaining_quantity = Quantity(0.0);
    order.avg_fill_price = fill_price;
    order.last_updated = clock_->now();
    registry_.update_order(order);

    // Применить fill к портфелю
    registry_.mark_fill_applied(order_id);
    apply_fill_to_portfolio(order);

    if (metrics_) {
        metrics_->counter("execution_fills_total", {})->increment();
    }

    logger_->info("FillProcessor", "Market fill обработан",
        {{"order_id", order_id.get()},
         {"filled_qty", std::to_string(filled_qty.get())},
         {"fill_price", std::to_string(fill_price.get())},
         {"symbol", order.symbol.get()}});

    return true;
}

bool FillProcessor::process_fill_event(const FillEvent& fill) {
    auto opt = registry_.get_order(fill.order_id);
    if (!opt) {
        logger_->warn("FillProcessor", "Fill event для неизвестного ордера",
            {{"order_id", fill.order_id.get()},
             {"trade_id", fill.trade_id}});
        return false;
    }

    auto order = *opt;

    // Обновить volumes
    double prev_cost = order.filled_quantity.get() * order.avg_fill_price.get();
    double fill_cost = fill.fill_quantity.get() * fill.fill_price.get();
    double new_filled = order.filled_quantity.get() + fill.fill_quantity.get();

    order.filled_quantity = Quantity(new_filled);
    order.remaining_quantity = Quantity(order.original_quantity.get() - new_filled);
    if (new_filled > 0.0) {
        order.avg_fill_price = Price((prev_cost + fill_cost) / new_filled);
    }

    // Записать fill event
    order.execution_info.fills.push_back(fill);

    // Latency
    if (order.execution_info.fills.size() == 1 && order.created_at.get() > 0) {
        order.execution_info.first_fill_latency_ms =
            (fill.occurred_at.get() - order.created_at.get()) / 1'000'000;
    }

    // Slippage
    if (order.execution_info.expected_fill_price.get() > 0.0) {
        order.execution_info.realized_slippage =
            (order.avg_fill_price.get() - order.execution_info.expected_fill_price.get())
            / order.execution_info.expected_fill_price.get();
    }

    order.last_updated = clock_->now();

    // FSM transition
    if (new_filled >= order.original_quantity.get()) {
        registry_.transition(fill.order_id, OrderState::Filled, "Full fill (fill event)");
        order.state = OrderState::Filled;

        // Portfolio update
        if (!registry_.is_fill_applied(fill.order_id)) {
            registry_.mark_fill_applied(fill.order_id);
            apply_fill_to_portfolio(order);
        }
    } else {
        registry_.transition(fill.order_id, OrderState::PartiallyFilled,
                             "Partial fill (fill event)");
        order.state = OrderState::PartiallyFilled;
    }

    registry_.update_order(order);

    if (metrics_) {
        metrics_->counter("order_fills_total", {})->increment();
    }

    logger_->debug("FillProcessor", "Fill event обработан",
        {{"order_id", fill.order_id.get()},
         {"fill_qty", std::to_string(fill.fill_quantity.get())},
         {"fill_price", std::to_string(fill.fill_price.get())},
         {"cumulative", std::to_string(new_filled)},
         {"trade_id", fill.trade_id}});

    return true;
}

bool FillProcessor::process_order_fill(const OrderId& order_id,
                                        Quantity filled_qty, Price fill_price) {
    auto opt = registry_.get_order(order_id);
    if (!opt) return false;

    auto order = *opt;
    order.filled_quantity = filled_qty;
    order.remaining_quantity = Quantity(order.original_quantity.get() - filled_qty.get());
    if (fill_price.get() > 0.0) {
        order.avg_fill_price = fill_price;
    }
    order.last_updated = clock_->now();
    registry_.update_order(order);

    // Portfolio update при полном fill
    if (order.state == OrderState::Filled && portfolio_) {
        if (!registry_.is_fill_applied(order_id)) {
            registry_.mark_fill_applied(order_id);
            apply_fill_to_portfolio(order);
        }
    }

    return true;
}

void FillProcessor::apply_fill_to_portfolio(const OrderRecord& order) {
    if (!portfolio_) return;

    bool is_close = (order.trade_side == TradeSide::Close);
    bool is_open = (order.trade_side == TradeSide::Open);

    if (is_close || (!is_open && order.side == Side::Sell)) {
        // SELL / Close: уменьшить позицию
        double gross_pnl = compute_gross_pnl(order);
        portfolio_->reduce_position(order.symbol, order.filled_quantity,
                                    order.avg_fill_price, gross_pnl);
        double sell_fee = order.avg_fill_price.get() * order.filled_quantity.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, sell_fee, order.order_id);

        logger_->info("FillProcessor", "Позиция уменьшена (sell fill)",
            {{"symbol", order.symbol.get()},
             {"qty", std::to_string(order.filled_quantity.get())},
             {"gross_pnl", std::to_string(gross_pnl)},
             {"sell_fee", std::to_string(sell_fee)}});
    } else {
        // BUY / Open: открыть позицию
        portfolio::Position pos;
        pos.symbol = order.symbol;
        pos.side = order.side;
        pos.size = order.filled_quantity;
        pos.avg_entry_price = order.avg_fill_price;
        pos.current_price = order.avg_fill_price;
        pos.notional = NotionalValue(
            order.filled_quantity.get() * order.avg_fill_price.get());
        pos.strategy_id = order.strategy_id;
        pos.opened_at = clock_->now();
        pos.updated_at = clock_->now();
        portfolio_->open_position(pos);
        portfolio_->release_cash(order.order_id);
        double buy_fee = order.filled_quantity.get() * order.avg_fill_price.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, buy_fee, order.order_id);
    }
}

double FillProcessor::compute_gross_pnl(const OrderRecord& order) const {
    if (!portfolio_) return 0.0;

    auto existing = portfolio_->get_position(order.symbol);
    if (!existing) return 0.0;

    double entry = existing->avg_entry_price.get();
    double exit_p = order.avg_fill_price.get();
    double qty = order.filled_quantity.get();

    if (existing->side == Side::Buy) {
        return (exit_p - entry) * qty;  // Long profit
    } else {
        return (entry - exit_p) * qty;  // Short profit
    }
}

} // namespace tb::execution
