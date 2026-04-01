#include "execution/cancel/cancel_manager.hpp"

namespace tb::execution {

CancelManager::CancelManager(
    OrderRegistry& registry,
    std::shared_ptr<IOrderSubmitter> submitter,
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    const ExecutionConfig& config)
    : registry_(registry)
    , submitter_(std::move(submitter))
    , portfolio_(std::move(portfolio))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , config_(config)
{
}

VoidResult CancelManager::cancel_order(const OrderId& order_id) {
    if (do_cancel(order_id)) {
        return {};
    }
    return std::unexpected(TbError::ExecutionFailed);
}

std::vector<OrderId> CancelManager::cancel_all_for_symbol(const Symbol& symbol) {
    auto orders = registry_.orders_for_symbol(symbol);
    std::vector<OrderId> cancelled;

    for (const auto& order : orders) {
        if (order.is_active() && do_cancel(order.order_id)) {
            cancelled.push_back(order.order_id);
        }
    }

    logger_->info("CancelManager", "Cancel all for symbol",
        {{"symbol", symbol.get()},
         {"cancelled", std::to_string(cancelled.size())}});

    return cancelled;
}

std::vector<OrderId> CancelManager::cancel_all() {
    auto active = registry_.active_orders();
    std::vector<OrderId> cancelled;

    for (const auto& order : active) {
        if (do_cancel(order.order_id)) {
            cancelled.push_back(order.order_id);
        }
    }

    logger_->warn("CancelManager", "Cancel ALL ордеров",
        {{"cancelled", std::to_string(cancelled.size())}});

    return cancelled;
}

std::vector<OrderId> CancelManager::cancel_timed_out_orders(int64_t max_open_duration_ms) {
    auto timed_out = registry_.get_timed_out_orders(max_open_duration_ms);
    std::vector<OrderId> cancelled;

    for (const auto& oid : timed_out) {
        auto opt = registry_.get_order(oid);
        if (!opt) continue;

        // Skip orders already in terminal state
        if (opt->is_terminal()) continue;

        logger_->info("CancelManager", "Timeout cancel",
            {{"order_id", oid.get()},
             {"symbol", opt->symbol.get()}});

        if (do_cancel(oid)) {
            cancelled.push_back(oid);
        }
    }

    return cancelled;
}

bool CancelManager::cancel_remaining(const OrderId& order_id) {
    auto opt = registry_.get_order(order_id);
    if (!opt) return false;

    logger_->debug("CancelManager", "CancelRemaining: отмена остатка",
        {{"order_id", order_id.get()},
         {"remaining", std::to_string(opt->remaining_quantity.get())}});

    return do_cancel(order_id);
}

bool CancelManager::do_cancel(const OrderId& order_id) {
    auto opt = registry_.get_order(order_id);
    if (!opt) {
        logger_->warn("CancelManager", "Ордер не найден для отмены",
            {{"order_id", order_id.get()}});
        return false;
    }

    // FSM: перевести в CancelPending
    if (!registry_.transition(order_id, OrderState::CancelPending, "Cancel requested")) {
        logger_->warn("CancelManager", "Невозможно отменить ордер в текущем состоянии",
            {{"order_id", order_id.get()}});
        return false;
    }

    // Сетевой вызов: отправить cancel на биржу
    bool success = submitter_->cancel_order(opt->exchange_order_id, opt->symbol);

    if (success) {
        registry_.transition(order_id, OrderState::Cancelled, "Cancel confirmed");

        // Освободить cash
        release_cash_if_needed(*opt);

        logger_->info("CancelManager", "Ордер отменён",
            {{"order_id", order_id.get()},
             {"symbol", opt->symbol.get()}});
    } else {
        // §33: cancel не подтверждён — не считаем отменённым
        // Оставляем в CancelPending для recovery
        logger_->warn("CancelManager", "Cancel не подтверждён — оставляем в CancelPending",
            {{"order_id", order_id.get()},
             {"symbol", opt->symbol.get()}});
    }

    return success;
}

void CancelManager::release_cash_if_needed(const OrderRecord& order) {
    if (order.side == Side::Buy && portfolio_ && order.trade_side != TradeSide::Close) {
        portfolio_->release_cash(order.order_id);
    }
}

} // namespace tb::execution
