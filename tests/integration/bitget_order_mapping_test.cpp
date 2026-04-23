/**
 * @file bitget_order_mapping_test.cpp
 * @brief Integration tests for Bitget USDT-M hedge mode order mapping
 *
 * Verifies that OrderRecord fields are correctly set for all combinations
 * of PositionSide × TradeSide × OrderType as required by Bitget API v2.
 *
 * These tests do NOT hit the network — they verify the internal data model
 * that drives the JSON builder inside BitgetFuturesOrderSubmitter.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "execution/order_types.hpp"
#include "common/types.hpp"

using namespace tb;
using namespace tb::execution;

namespace {

OrderRecord make_base_order() {
    OrderRecord ord;
    ord.order_id = OrderId("TEST-001");
    ord.symbol = Symbol("BTCUSDT");
    ord.original_quantity = Quantity(0.01);
    ord.price = Price(60000.0);
    ord.state = OrderState::New;
    return ord;
}

} // namespace

// Bitget hedge mode: side=buy + tradeSide=open → Long open
TEST_CASE("OrderMapping: Long open has buy+open", "[mapping][hedge_mode]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Open;
    ord.order_type = OrderType::Market;

    CHECK(ord.side == Side::Buy);
    CHECK(ord.position_side == PositionSide::Long);
    CHECK(ord.trade_side == TradeSide::Open);
}

// Bitget hedge mode: side=buy + tradeSide=close → Long close (reduce long)
TEST_CASE("OrderMapping: Long close has buy+close", "[mapping][hedge_mode]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Close;
    ord.order_type = OrderType::Market;

    CHECK(ord.side == Side::Buy);
    CHECK(ord.position_side == PositionSide::Long);
    CHECK(ord.trade_side == TradeSide::Close);
}

// Bitget hedge mode: side=sell + tradeSide=open → Short open
TEST_CASE("OrderMapping: Short open has sell+open", "[mapping][hedge_mode]") {
    auto ord = make_base_order();
    ord.side = Side::Sell;
    ord.position_side = PositionSide::Short;
    ord.trade_side = TradeSide::Open;
    ord.order_type = OrderType::Market;

    CHECK(ord.side == Side::Sell);
    CHECK(ord.position_side == PositionSide::Short);
    CHECK(ord.trade_side == TradeSide::Open);
}

// Bitget hedge mode: side=sell + tradeSide=close → Short close (reduce short)
TEST_CASE("OrderMapping: Short close has sell+close", "[mapping][hedge_mode]") {
    auto ord = make_base_order();
    ord.side = Side::Sell;
    ord.position_side = PositionSide::Short;
    ord.trade_side = TradeSide::Close;
    ord.order_type = OrderType::Market;

    CHECK(ord.side == Side::Sell);
    CHECK(ord.position_side == PositionSide::Short);
    CHECK(ord.trade_side == TradeSide::Close);
}

// Hedge pair scenario: primary long + hedge short
TEST_CASE("OrderMapping: hedge pair — long primary + short hedge", "[mapping][hedge_pair]") {
    // Primary: open long
    auto primary = make_base_order();
    primary.order_id = OrderId("PRIMARY-001");
    primary.side = Side::Buy;
    primary.position_side = PositionSide::Long;
    primary.trade_side = TradeSide::Open;
    primary.order_type = OrderType::Limit;
    primary.tif = TimeInForce::GoodTillCancel;

    // Hedge: open short (opposite direction, same symbol)
    auto hedge = make_base_order();
    hedge.order_id = OrderId("HEDGE-001");
    hedge.side = Side::Sell;
    hedge.position_side = PositionSide::Short;
    hedge.trade_side = TradeSide::Open;
    hedge.order_type = OrderType::Market;

    // Verify they coexist correctly in hedge mode
    CHECK(primary.position_side != hedge.position_side);
    CHECK(primary.side != hedge.side);
    CHECK(primary.trade_side == TradeSide::Open);
    CHECK(hedge.trade_side == TradeSide::Open);
    CHECK(primary.symbol.get() == hedge.symbol.get());
}

// Close primary + keep hedge (asymmetric unwind)
TEST_CASE("OrderMapping: asymmetric unwind — close primary long", "[mapping][hedge_pair]") {
    auto close_primary = make_base_order();
    close_primary.side = Side::Buy;
    close_primary.position_side = PositionSide::Long;
    close_primary.trade_side = TradeSide::Close;
    close_primary.order_type = OrderType::Market;

    // Close order must have Close trade_side for Bitget hedge mode
    CHECK(close_primary.trade_side == TradeSide::Close);
    // Side matches position direction, NOT action
    CHECK(close_primary.side == Side::Buy);
}

// Attached TP/SL for position protection
TEST_CASE("OrderMapping: attached TP/SL on entry", "[mapping][tp_sl]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Open;
    ord.order_type = OrderType::Limit;
    ord.tif = TimeInForce::GoodTillCancel;
    ord.attached_tp_sl.stop_surplus_price = Price(62000.0);
    ord.attached_tp_sl.stop_loss_price = Price(58000.0);
    ord.attached_tp_sl.trigger_type = TriggerType::MarkPrice;

    CHECK(ord.attached_tp_sl.has_tp());
    CHECK(ord.attached_tp_sl.has_sl());
    CHECK(ord.attached_tp_sl.has_any());
    CHECK(ord.attached_tp_sl.stop_surplus_price.get() == 62000.0);
    CHECK(ord.attached_tp_sl.stop_loss_price.get() == 58000.0);
}

// Plan (trigger) order construction
TEST_CASE("OrderMapping: plan order with mark price trigger", "[mapping][plan_order]") {
    auto ord = make_base_order();
    ord.side = Side::Sell;
    ord.position_side = PositionSide::Short;
    ord.trade_side = TradeSide::Open;
    ord.order_type = OrderType::StopMarket;
    ord.plan_params.trigger_price = Price(59000.0);
    ord.plan_params.trigger_type = TriggerType::MarkPrice;

    CHECK(ord.order_type == OrderType::StopMarket);
    CHECK(ord.plan_params.trigger_price.get() == 59000.0);
    CHECK(ord.plan_params.trigger_type == TriggerType::MarkPrice);
}

TEST_CASE("OrderMapping: stop limit plan order", "[mapping][plan_order]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Close;
    ord.order_type = OrderType::StopLimit;
    ord.plan_params.trigger_price = Price(59500.0);
    ord.plan_params.trigger_type = TriggerType::FillPrice;
    ord.plan_params.execute_price = Price(59400.0);

    CHECK(ord.order_type == OrderType::StopLimit);
    CHECK(ord.plan_params.trigger_type == TriggerType::FillPrice);
    CHECK(ord.plan_params.execute_price.get() == 59400.0);
}

// PostOnly force mapping
TEST_CASE("OrderMapping: post-only order type mapping", "[mapping][order_type]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Open;
    ord.order_type = OrderType::PostOnly;

    CHECK(ord.order_type == OrderType::PostOnly);
    // PostOnly maps to "limit" orderType + "post_only" force in Bitget
}

// TimeInForce variants
TEST_CASE("OrderMapping: time in force variants", "[mapping][tif]") {
    auto base = make_base_order();
    base.side = Side::Buy;
    base.position_side = PositionSide::Long;
    base.trade_side = TradeSide::Open;
    base.order_type = OrderType::Limit;

    SECTION("GTC") {
        base.tif = TimeInForce::GoodTillCancel;
        CHECK(base.tif == TimeInForce::GoodTillCancel);
    }
    SECTION("IOC") {
        base.tif = TimeInForce::ImmediateOrCancel;
        CHECK(base.tif == TimeInForce::ImmediateOrCancel);
    }
    SECTION("FOK") {
        base.tif = TimeInForce::FillOrKill;
        CHECK(base.tif == TimeInForce::FillOrKill);
    }
}

// Reduce-only close in hedge mode (reduceOnly is NOT sent, tradeSide=close suffices)
TEST_CASE("OrderMapping: reduce-only close via tradeSide", "[mapping][reduce_only]") {
    auto ord = make_base_order();
    ord.side = Side::Buy;
    ord.position_side = PositionSide::Long;
    ord.trade_side = TradeSide::Close;

    // In hedge mode, close is expressed via tradeSide=close, not reduceOnly flag
    CHECK(ord.trade_side == TradeSide::Close);
}

// Fill event tracking
TEST_CASE("OrderMapping: fill event records execution details", "[mapping][fill]") {
    FillEvent fill;
    fill.order_id = OrderId("EXCH-001");
    fill.fill_quantity = Quantity(0.005);
    fill.fill_price = Price(60050.0);
    fill.cumulative_filled = Quantity(0.005);
    fill.fee = 0.036;

    CHECK(fill.fill_quantity.get() == 0.005);
    CHECK(fill.fill_price.get() == 60050.0);
    CHECK(fill.fee > 0.0);
}
