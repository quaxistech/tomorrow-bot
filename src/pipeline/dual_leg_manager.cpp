/**
 * @file dual_leg_manager.cpp
 * @brief DualLegManager — coordinated long+short pair execution (Phase E)
 */

#include "dual_leg_manager.hpp"
#include "exchange/bitget/bitget_futures_order_submitter.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include <boost/json.hpp>
#include <cmath>

namespace tb::pipeline {

static constexpr char kComp[] = "DualLeg";

DualLegManager::DualLegManager(
    DualLegConfig config,
    std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter,
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , submitter_(std::move(submitter))
    , rest_client_(std::move(rest_client))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{
}

void DualLegManager::compute_tpsl(double entry_price, PositionSide side,
                                   double& tp_price, double& sl_price) const {
    if (side == PositionSide::Long) {
        tp_price = entry_price * (1.0 + config_.tpsl_take_profit_pct / 100.0);
        sl_price = entry_price * (1.0 - config_.tpsl_stop_loss_pct / 100.0);
    } else {
        tp_price = entry_price * (1.0 - config_.tpsl_take_profit_pct / 100.0);
        sl_price = entry_price * (1.0 + config_.tpsl_stop_loss_pct / 100.0);
    }
}

PairEntryResult DualLegManager::enter_pair(const Symbol& symbol,
                                            const LegSpec& long_spec,
                                            const LegSpec& short_spec) {
    PairEntryResult result;
    auto now_ns = clock_->now().get();

    // Build long leg order
    execution::OrderRecord long_order;
    long_order.symbol = symbol;
    long_order.side = Side::Buy;
    long_order.position_side = PositionSide::Long;
    long_order.trade_side = TradeSide::Open;
    long_order.order_type = OrderType::Market;
    long_order.tif = TimeInForce::ImmediateOrCancel;
    long_order.original_quantity = Quantity(long_spec.size);
    long_order.remaining_quantity = Quantity(long_spec.size);
    long_order.price = Price(long_spec.entry_price_hint);

    // Attach server-side TPSL if configured
    if (config_.attach_server_tpsl && long_spec.tp_price > 0.0) {
        long_order.attached_tp_sl.stop_surplus_price = Price(long_spec.tp_price);
        long_order.attached_tp_sl.stop_loss_price = Price(long_spec.sl_price);
        long_order.attached_tp_sl.trigger_type = execution::TriggerType::MarkPrice;
    }

    logger_->info(kComp, "Entering pair: submitting LONG leg",
        {{"symbol", symbol.get()},
         {"size", std::to_string(long_spec.size)},
         {"tp", std::to_string(long_spec.tp_price)},
         {"sl", std::to_string(long_spec.sl_price)}});

    // 1. Submit long leg
    auto long_result = submitter_->submit_order(long_order);
    if (!long_result.success) {
        result.error = "Long leg failed: " + long_result.error_message;
        logger_->error(kComp, result.error, {});
        return result;
    }

    long_leg_.state = LegState::PendingEntry;
    long_leg_.side = PositionSide::Long;
    long_leg_.order_id = long_result.order_id;
    long_leg_.exchange_order_id = long_result.exchange_order_id;
    long_leg_.size = long_spec.size;
    long_leg_.entry_time_ns = now_ns;
    long_leg_.has_server_tp = (long_spec.tp_price > 0.0);
    long_leg_.has_server_sl = (long_spec.sl_price > 0.0);

    // 2. Submit short leg
    execution::OrderRecord short_order;
    short_order.symbol = symbol;
    short_order.side = Side::Sell;
    short_order.position_side = PositionSide::Short;
    short_order.trade_side = TradeSide::Open;
    short_order.order_type = OrderType::Market;
    short_order.tif = TimeInForce::ImmediateOrCancel;
    short_order.original_quantity = Quantity(short_spec.size);
    short_order.remaining_quantity = Quantity(short_spec.size);
    short_order.price = Price(short_spec.entry_price_hint);

    if (config_.attach_server_tpsl && short_spec.tp_price > 0.0) {
        short_order.attached_tp_sl.stop_surplus_price = Price(short_spec.tp_price);
        short_order.attached_tp_sl.stop_loss_price = Price(short_spec.sl_price);
        short_order.attached_tp_sl.trigger_type = execution::TriggerType::MarkPrice;
    }

    logger_->info(kComp, "Entering pair: submitting SHORT leg",
        {{"symbol", symbol.get()},
         {"size", std::to_string(short_spec.size)},
         {"tp", std::to_string(short_spec.tp_price)},
         {"sl", std::to_string(short_spec.sl_price)}});

    auto short_result = submitter_->submit_order(short_order);
    if (!short_result.success) {
        // Second leg failed — cancel first leg; if cancel fails (already filled),
        // close the orphan position with a market order to prevent dangling exposure.
        logger_->error(kComp, "Short leg failed, unwinding long leg",
            {{"error", short_result.error_message}});

        auto cancel_ok = submitter_->cancel_order(long_result.exchange_order_id, symbol);
        if (!cancel_ok) {
            // Order was already filled — close the orphan position
            logger_->warn(kComp, "Cancel failed (likely filled), closing orphan long leg",
                {{"exchange_oid", long_result.exchange_order_id.get()}});
            close_leg(symbol, PositionSide::Long, long_spec.size, "orphan_unwind");
        }

        long_leg_ = {};
        result.error = "Short leg failed: " + short_result.error_message;
        return result;
    }

    short_leg_.state = LegState::PendingEntry;
    short_leg_.side = PositionSide::Short;
    short_leg_.order_id = short_result.order_id;
    short_leg_.exchange_order_id = short_result.exchange_order_id;
    short_leg_.size = short_spec.size;
    short_leg_.entry_time_ns = now_ns;
    short_leg_.has_server_tp = (short_spec.tp_price > 0.0);
    short_leg_.has_server_sl = (short_spec.sl_price > 0.0);

    result.success = true;
    result.long_leg = long_leg_;
    result.short_leg = short_leg_;

    logger_->info(kComp, "Pair entry submitted successfully",
        {{"symbol", symbol.get()},
         {"long_oid", long_result.exchange_order_id.get()},
         {"short_oid", short_result.exchange_order_id.get()}});

    return result;
}

ReversalResult DualLegManager::reverse_leg(const Symbol& symbol,
                                            PositionSide current_side,
                                            double size) {
    ReversalResult result;
    int64_t now_ns = clock_->now().get();

    // Cooldown check
    if (now_ns - last_reversal_ns_ < static_cast<int64_t>(config_.reversal_cooldown_ms) * 1'000'000LL) {
        result.error = "Reversal cooldown active";
        return result;
    }

    // Try atomic reversal via click-backhand API
    if (config_.use_reversal_api && rest_client_) {
        try {
            boost::json::object body;
            body["symbol"] = symbol.get();
            body["productType"] = "USDT-FUTURES";
            body["marginCoin"] = "USDT";
            body["size"] = std::to_string(size);

            // click-backhand flips the position atomically
            auto resp = rest_client_->post(
                "/api/v2/mix/order/click-backhand",
                boost::json::serialize(body));

            if (resp.success) {
                auto json = boost::json::parse(resp.body);
                auto& root = json.as_object();
                std::string code = std::string(root.at("code").as_string());

                if (code == "00000") {
                    result.success = true;
                    last_reversal_ns_ = now_ns;

                    // Extract new order ID if available
                    if (root.contains("data") && root.at("data").is_object()) {
                        auto& data = root.at("data").as_object();
                        if (data.contains("orderId")) {
                            result.new_order_id = OrderId(
                                std::string(data.at("orderId").as_string()));
                        }
                    }

                    logger_->info(kComp, "Reversal API succeeded",
                        {{"symbol", symbol.get()},
                         {"from_side", current_side == PositionSide::Long ? "long" : "short"},
                         {"size", std::to_string(size)}});

                    return result;
                }

                logger_->warn(kComp, "Reversal API returned error code, falling back",
                    {{"code", code}, {"body", resp.body.substr(0, 256)}});
            }
        } catch (const std::exception& ex) {
            logger_->warn(kComp, "Reversal API exception, falling back to close+reopen",
                {{"error", std::string(ex.what())}});
        }
    }

    // Fallback: close current + open opposite
    bool closed = close_leg(symbol, current_side, size, "reversal_close");
    if (!closed) {
        result.error = "Failed to close leg for reversal";
        return result;
    }

    // Open opposite
    PositionSide new_side = (current_side == PositionSide::Long)
        ? PositionSide::Short : PositionSide::Long;

    execution::OrderRecord reopen;
    reopen.symbol = symbol;
    reopen.side = (new_side == PositionSide::Long) ? Side::Buy : Side::Sell;
    reopen.position_side = new_side;
    reopen.trade_side = TradeSide::Open;
    reopen.order_type = OrderType::Market;
    reopen.tif = TimeInForce::ImmediateOrCancel;
    reopen.original_quantity = Quantity(size);
    reopen.remaining_quantity = Quantity(size);

    auto submit_result = submitter_->submit_order(reopen);
    if (!submit_result.success) {
        result.error = "Reversal reopen failed: " + submit_result.error_message;
        logger_->error(kComp, result.error, {});
        return result;
    }

    result.success = true;
    result.new_order_id = submit_result.exchange_order_id;
    last_reversal_ns_ = now_ns;

    logger_->info(kComp, "Reversal via close+reopen succeeded",
        {{"symbol", symbol.get()},
         {"new_side", new_side == PositionSide::Long ? "long" : "short"},
         {"size", std::to_string(size)}});

    return result;
}

bool DualLegManager::close_leg(const Symbol& symbol, PositionSide side,
                                double size, const std::string& reason) {
    execution::OrderRecord close_order;
    close_order.symbol = symbol;
    // Close Long = Sell, Close Short = Buy (hedge-mode convention)
    close_order.side = (side == PositionSide::Long) ? Side::Sell : Side::Buy;
    close_order.position_side = side;
    close_order.trade_side = TradeSide::Close;
    close_order.order_type = OrderType::Market;
    close_order.tif = TimeInForce::ImmediateOrCancel;
    close_order.original_quantity = Quantity(size);
    close_order.remaining_quantity = Quantity(size);

    auto result = submitter_->submit_order(close_order);
    if (!result.success) {
        logger_->error(kComp, "Failed to close leg",
            {{"side", side == PositionSide::Long ? "long" : "short"},
             {"reason", reason},
             {"error", result.error_message}});
        return false;
    }

    // Update leg state
    auto& leg = (side == PositionSide::Long) ? long_leg_ : short_leg_;
    leg.state = LegState::PendingExit;

    logger_->info(kComp, "Leg close submitted",
        {{"side", side == PositionSide::Long ? "long" : "short"},
         {"size", std::to_string(size)},
         {"reason", reason}});

    return true;
}

bool DualLegManager::close_both(const Symbol& symbol, const std::string& reason) {
    bool ok = true;

    if (long_leg_.state == LegState::Active) {
        if (!close_leg(symbol, PositionSide::Long, long_leg_.size, reason)) {
            ok = false;
        }
    }

    if (short_leg_.state == LegState::Active) {
        if (!close_leg(symbol, PositionSide::Short, short_leg_.size, reason)) {
            ok = false;
        }
    }

    if (ok) {
        logger_->info(kComp, "Both legs close submitted", {{"reason", reason}});
    }

    return ok;
}

bool DualLegManager::is_carry_too_expensive(double funding_rate) const {
    // In a hedge pair, one side pays and other receives — but net carry depends on
    // the size difference. For equal-size pair, the net funding is near zero.
    // Only flag if funding > threshold (typically during extreme market conditions)
    return std::abs(funding_rate) > config_.max_funding_rate_abs;
}

bool DualLegManager::has_active_pair() const {
    return long_leg_.state == LegState::Active && short_leg_.state == LegState::Active;
}

void DualLegManager::reset() {
    long_leg_ = {};
    short_leg_ = {};
}

void DualLegManager::update_leg(PositionSide side, double fill_price, double fill_qty,
                                 int64_t fill_time_ns) {
    auto& leg = (side == PositionSide::Long) ? long_leg_ : short_leg_;
    leg.entry_price = fill_price;
    leg.size = fill_qty;
    leg.entry_time_ns = fill_time_ns;
    leg.state = LegState::Active;
}

} // namespace tb::pipeline
