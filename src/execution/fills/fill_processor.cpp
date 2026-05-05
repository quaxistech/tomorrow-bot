#include "execution/fills/fill_processor.hpp"
#include "common/constants.hpp"
#include "persistence/persistence_types.hpp"
#include <algorithm>
#include <cmath>
#include <format>

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

    // BUG-S29-02: validate fill_price before any processing
    if (!std::isfinite(fill_price.get()) || fill_price.get() <= 0.0) {
        logger_->error("FillProcessor", "process_market_fill: невалидная fill_price, пропуск",
            {{"order_id", order_id.get()},
             {"fill_price", std::to_string(fill_price.get())}});
        return false;
    }
    // BUG-S29-03: validate filled_qty
    if (!std::isfinite(filled_qty.get()) || filled_qty.get() <= 0.0) {
        logger_->error("FillProcessor", "process_market_fill: невалидная filled_qty, пропуск",
            {{"order_id", order_id.get()},
             {"filled_qty", std::to_string(filled_qty.get())}});
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

    // Применить fill к портфелю (HIGH-10: mark only after successful portfolio update)
    apply_fill_to_portfolio(order);
    registry_.mark_fill_applied(order_id);

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
    // BUG-S29-04: atomic check-and-mark under one lock to prevent TOCTOU race
    // where two threads (WS + REST) both pass is_trade_id_seen() before either
    // calls mark_trade_id_seen() — causing double portfolio application.
    if (!fill.trade_id.empty() && registry_.check_and_mark_trade_id_seen(fill.trade_id)) {
        logger_->debug("FillProcessor", "Fill event дубликат по tradeId",
            {{"order_id", fill.order_id.get()},
             {"trade_id", fill.trade_id}});
        if (metrics_) {
            metrics_->counter("tb_fill_duplicates_total")->increment();
        }
        return false;
    }

    auto opt = registry_.get_order(fill.order_id);
    // effective_id = internal order ID for all registry operations
    // (fill.order_id may be the exchange order ID from WS)
    OrderId effective_id = fill.order_id;
    if (!opt) {
        // Fallback: WS fill events use exchange_order_id, try reverse lookup
        opt = registry_.get_order_by_exchange_id(fill.order_id);
        if (opt) {
            effective_id = opt->order_id;  // resolve to internal ID
        }
    }
    if (!opt) {
        logger_->debug("FillProcessor", "Fill event для неизвестного ордера (WS race)",
            {{"order_id", fill.order_id.get()},
             {"trade_id", fill.trade_id}});
        return false;
    }

    // Guard: if WS fill has price=0 or NaN, skip ALL processing. Don't mark tradeId,
    // don't transition FSM, don't mark fill_applied. The REST path
    // (process_market_fill) will handle this fill with the correct price.
    // Bitget WS fill channel sometimes sends fillPx=0; marking fill_applied
    // here would block the REST path and leave the portfolio stale.
    // BUG-S29-01: NaN comparison is always false, so use isfinite instead of <= 0.0.
    if (!std::isfinite(fill.fill_price.get()) || fill.fill_price.get() <= 0.0) {
        logger_->warn("FillProcessor", "WS fill с fill_price <= 0 — пропуск, ждём REST подтверждения",
            {{"order_id", effective_id.get()},
             {"exchange_id", fill.order_id.get()},
             {"fill_qty", std::to_string(fill.fill_quantity.get())},
             {"trade_id", fill.trade_id}});
        return false;
    }

    // tradeId already marked seen by check_and_mark_trade_id_seen() above

    auto order = *opt;

    // Compute new cumulative fill from delta
    double prev_cost = order.filled_quantity.get() * order.avg_fill_price.get();
    double fill_cost = fill.fill_quantity.get() * fill.fill_price.get();
    double new_filled = order.filled_quantity.get() + fill.fill_quantity.get();

    order.filled_quantity = Quantity(new_filled);
    order.remaining_quantity = Quantity(
        std::max(0.0, order.original_quantity.get() - new_filled));
    if (new_filled > 0.0) {
        order.avg_fill_price = Price((prev_cost + fill_cost) / new_filled);
    }

    // Append to per-order fill ledger
    order.execution_info.fills.push_back(fill);

    // Latency (first fill only)
    if (order.execution_info.fills.size() == 1 && order.created_at.get() > 0) {
        order.execution_info.first_fill_latency_ms =
            (fill.occurred_at.get() - order.created_at.get()) / 1'000'000;
    }

    // Slippage tracking
    if (order.execution_info.expected_fill_price.get() > 0.0) {
        order.execution_info.realized_slippage =
            (order.avg_fill_price.get() - order.execution_info.expected_fill_price.get())
            / order.execution_info.expected_fill_price.get();
    }

    order.last_updated = clock_->now();

    // FSM transition (use effective_id = internal order ID)
    bool is_full_fill = (new_filled >= order.original_quantity.get() * 0.999);
    if (is_full_fill) {
        registry_.transition(effective_id, OrderState::Filled, "Full fill (fill event)");
        order.state = OrderState::Filled;
    } else {
        registry_.transition(effective_id, OrderState::PartiallyFilled,
                             "Partial fill (fill event)");
        order.state = OrderState::PartiallyFilled;
    }

    registry_.update_order(order);

    // Incremental portfolio update: apply this individual fill delta immediately.
    // This replaces the old approach of waiting for full fill.
    if (portfolio_ && fill.fill_quantity.get() > 0.0) {
        apply_incremental_fill(order, fill);
    }

    // Mark fill as tracked by WS path on EVERY fill (partial or full).
    // This prevents REST process_order_fill / process_market_fill from re-applying.
    // CRITICAL: use effective_id (internal order ID), NOT fill.order_id (may be exchange ID).
    // Without this matching, the idempotency guard in process_market_fill wouldn't
    // recognise the already-applied fill because it checks by internal ID.
    registry_.mark_fill_applied(effective_id);

    if (!is_full_fill) {
        // Partial fill: check if CancelRemaining policy applies
        auto cancel_action = check_cancel_remaining(effective_id);
        if (cancel_action.needed) {
            logger_->info("FillProcessor", "CancelRemaining policy triggered",
                {{"order_id", effective_id.get()},
                 {"filled", std::to_string(new_filled)},
                 {"original", std::to_string(order.original_quantity.get())}});
            // Note: actual cancel is returned via check_cancel_remaining;
            // caller (ExecutionEngine) is responsible for executing the cancel.
        }
    }

    if (metrics_) {
        metrics_->counter("order_fills_total", {})->increment();
    }

    logger_->debug("FillProcessor", "Fill event обработан",
        {{"order_id", effective_id.get()},
         {"exchange_id", fill.order_id.get()},
         {"fill_qty", std::to_string(fill.fill_quantity.get())},
         {"fill_price", std::to_string(fill.fill_price.get())},
         {"cumulative", std::to_string(new_filled)},
         {"trade_id", fill.trade_id}});

    return true;
}

bool FillProcessor::process_order_fill(const OrderId& order_id,
                                        Quantity filled_qty, Price fill_price) {
    // This is called from on_order_update (REST poll / WS order channel).
    // If fill events already processed this order incrementally via process_fill_event,
    // the fill_applied flag will be set. In that case, we only reconcile the
    // cumulative filled_qty (take max to avoid going backward) and skip portfolio ops
    // to prevent double-counting.
    auto opt = registry_.get_order(order_id);
    if (!opt) return false;

    auto order = *opt;
    bool already_applied = registry_.is_fill_applied(order_id);

    // Take max of reported vs tracked fill to handle out-of-order delivery
    double reconciled_filled = std::max(order.filled_quantity.get(), filled_qty.get());
    order.filled_quantity = Quantity(reconciled_filled);
    order.remaining_quantity = Quantity(
        std::max(0.0, order.original_quantity.get() - reconciled_filled));
    if (fill_price.get() > 0.0 && !already_applied) {
        // Only overwrite avg price if fills weren't already tracked incrementally
        order.avg_fill_price = fill_price;
    }
    order.last_updated = clock_->now();
    registry_.update_order(order);

    // Portfolio update only if fills weren't already applied via process_fill_event
    if (order.state == OrderState::Filled && portfolio_ && !already_applied) {
        registry_.mark_fill_applied(order_id);
        apply_fill_to_portfolio(order);
    }

    return true;
}

void FillProcessor::apply_fill_to_portfolio(const OrderRecord& order) {
    if (!portfolio_) return;

    // Фьючерсы USDT-M: логика определяется исключительно по trade_side.
    // TradeSide::Close — уменьшение/закрытие позиции (long или short).
    // TradeSide::Open  — открытие/увеличение позиции (long или short).
    if (order.trade_side == TradeSide::Close) {
        double gross_pnl = compute_gross_pnl(order);
        portfolio_->reduce_position(order.symbol, order.position_side, order.filled_quantity,
                                    order.avg_fill_price, gross_pnl);
        double fee = order.avg_fill_price.get() * order.filled_quantity.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, fee, order.order_id);
        journal_fee_event(order.symbol, order.order_id, fee,
                          order.avg_fill_price.get(), order.filled_quantity.get());

        logger_->info("FillProcessor", "Позиция уменьшена (close fill)",
            {{"symbol", order.symbol.get()},
             {"position_side", order.position_side == PositionSide::Long ? "Long" : "Short"},
             {"qty", std::to_string(order.filled_quantity.get())},
             {"gross_pnl", std::to_string(gross_pnl)},
             {"fee", std::to_string(fee)}});
    } else {
        // TradeSide::Open — открытие позиции (Long или Short)
        portfolio::Position pos;
        pos.symbol = order.symbol;
        pos.side = order.side;
        pos.position_side = order.position_side; // BUG-S8-05: hedge-mode requires position_side
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
        double fee = order.filled_quantity.get() * order.avg_fill_price.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, fee, order.order_id);
        journal_fee_event(order.symbol, order.order_id, fee,
                          order.avg_fill_price.get(), order.filled_quantity.get());
    }
}

void FillProcessor::apply_incremental_fill(const OrderRecord& order, const FillEvent& fill) {
    if (!portfolio_) return;

    Quantity fill_qty = fill.fill_quantity;
    Price fill_price = fill.fill_price;

    // Guard: skip portfolio update if fill_price is zero/negative.
    // A corrupted WS fill with price=0 would create a position with entry=0,
    // causing incorrect PnL and phantom-position cascading.
    if (fill_price.get() <= 0.0) {
        logger_->warn("FillProcessor", "Пропуск apply_incremental_fill: fill_price <= 0",
            {{"order_id", order.order_id.get()},
             {"fill_qty", std::to_string(fill_qty.get())},
             {"fill_price", std::to_string(fill_price.get())},
             {"symbol", order.symbol.get()}});
        return;
    }

    if (order.trade_side == TradeSide::Close) {
        double pnl = compute_incremental_pnl(order, fill_qty, fill_price);
        portfolio_->reduce_position(order.symbol, order.position_side, fill_qty,
                                    fill_price, pnl);
        double fee = fill_price.get() * fill_qty.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, fee, order.order_id);
        journal_fee_event(order.symbol, order.order_id, fee,
                          fill_price.get(), fill_qty.get());

        logger_->info("FillProcessor", "Инкрементальный close fill",
            {{"symbol", order.symbol.get()},
             {"position_side", order.position_side == PositionSide::Long ? "Long" : "Short"},
             {"qty", std::to_string(fill_qty.get())},
             {"price", std::to_string(fill_price.get())},
             {"pnl", std::to_string(pnl)}});
    } else {
        // TradeSide::Open — increase position
        portfolio::Position pos;
        pos.symbol = order.symbol;
        pos.side = order.side;
        pos.position_side = order.position_side; // BUG-S8-05: hedge-mode requires position_side
        pos.size = fill_qty;
        pos.avg_entry_price = fill_price;
        pos.current_price = fill_price;
        pos.notional = NotionalValue(fill_qty.get() * fill_price.get());
        pos.strategy_id = order.strategy_id;
        pos.opened_at = clock_->now();
        pos.updated_at = clock_->now();
        portfolio_->open_position(pos);
        portfolio_->release_cash(order.order_id);
        double fee = fill_qty.get() * fill_price.get() * kDefaultTakerFeePct;
        portfolio_->record_fee(order.symbol, fee, order.order_id);
        journal_fee_event(order.symbol, order.order_id, fee,
                          fill_price.get(), fill_qty.get());
    }
}

double FillProcessor::compute_incremental_pnl(const OrderRecord& order,
                                               Quantity fill_qty, Price fill_price) const {
    if (!portfolio_) return 0.0;

    auto existing = portfolio_->get_position(order.symbol, order.position_side);
    if (!existing) return 0.0;

    double entry = existing->avg_entry_price.get();
    double exit_p = fill_price.get();
    double qty = fill_qty.get();

    if (order.position_side == PositionSide::Long) {
        return (exit_p - entry) * qty;
    } else {
        return (entry - exit_p) * qty;
    }
}

double FillProcessor::compute_gross_pnl(const OrderRecord& order) const {
    if (!portfolio_) return 0.0;

    auto existing = portfolio_->get_position(order.symbol, order.position_side);
    if (!existing) return 0.0;

    double entry = existing->avg_entry_price.get();
    double exit_p = order.avg_fill_price.get();
    double qty = order.filled_quantity.get();

    // PnL определяется по position_side ордера (Long/Short),
    // а не по side (Buy/Sell), т.к. для фьючерсов Buy может быть
    // как открытием Long, так и закрытием Short.
    if (order.position_side == PositionSide::Long) {
        return (exit_p - entry) * qty;  // Long: прибыль при росте
    } else {
        return (entry - exit_p) * qty;  // Short: прибыль при падении
    }
}

void FillProcessor::journal_fee_event(const Symbol& symbol, const OrderId& order_id,
                                       double fee, double fill_price, double fill_qty)
{
    if (!journal_) return;

    // Build a minimal JSON payload describing the fee charge.
    // Using std::format with escaped literals avoids raw string concatenation bugs.
    auto ts = clock_ ? clock_->now().get() : int64_t{0};
    auto payload = std::format(
        R"({{"event":"FeeCharged","symbol":"{}","order_id":"{}","fee":{:.8f},"fill_price":{:.8f},"fill_qty":{:.8f},"ts":{}}})",
        symbol.get(), order_id.get(), fee, fill_price, fill_qty, ts);

    auto result = journal_->append(
        persistence::JournalEntryType::PortfolioChange,
        payload,
        CorrelationId{order_id.get()});

    if (!result && logger_) {
        logger_->warn("FillProcessor",
            "Не удалось записать FeeCharged в журнал",
            {{"order_id", order_id.get()}, {"fee", std::to_string(fee)}});
    }
}

FillProcessor::CancelRemainingAction FillProcessor::check_cancel_remaining(
    const OrderId& order_id) const
{
    CancelRemainingAction action;
    auto opt = registry_.get_order(order_id);
    if (!opt) return action;

    const auto& order = *opt;

    // Only trigger for CancelRemaining policy on partially filled orders
    if (order.execution_info.fill_policy != PartialFillPolicy::CancelRemaining) {
        return action;
    }
    if (order.state != OrderState::PartiallyFilled) {
        return action;
    }
    if (order.remaining_quantity.get() <= 0.0) {
        return action;
    }

    action.needed = true;
    action.order_id = order.order_id;
    action.exchange_order_id = order.exchange_order_id;
    action.symbol = order.symbol;
    action.side = order.side;
    return action;
}

} // namespace tb::execution
