/**
 * @file pre_trade_gates_test.cpp
 * @brief Тесты для FreshnessGate и NetRRGate (edge-31 TPSL refactor, Phase 2).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pipeline/pre_trade_gates.hpp"

using namespace tb;
using namespace tb::pipeline;
using namespace tb::strategy;
using namespace tb::features;
using namespace tb::config;
using namespace Catch::Matchers;

namespace {

PreTradeGatesConfig default_cfg() {
    PreTradeGatesConfig c;
    c.freshness_enabled = true;
    c.net_rr_enabled = true;
    c.max_signal_age_ms = 500;
    c.max_adverse_price_drift_bps = 8.0;
    c.max_spread_widen_pct = 50.0;
    c.min_depth_remain_pct = 50.0;
    c.min_net_rr = 0.5;
    c.assumed_slippage_bps_per_leg = 2.0;
    c.taker_fee_bps = 6.0;
    c.maker_fee_bps = 2.0;
    c.include_funding_cost = false;
    c.assumed_hold_minutes = 2.0;
    return c;
}

TradeIntent make_entry_intent_with_snapshot() {
    TradeIntent intent;
    intent.symbol = Symbol("BTCUSDT");
    intent.side = Side::Buy;
    intent.position_side = PositionSide::Long;
    intent.trade_side = TradeSide::Open;
    intent.signal_intent = SignalIntent::LongEntry;
    intent.suggested_quantity = Quantity(0.01);
    intent.signal_snapshot_ts_ns = 1'000'000'000LL;  // t0 = 1.0s
    intent.signal_snapshot_mid = 50000.0;
    intent.signal_snapshot_spread_bps = 4.0;
    intent.signal_snapshot_depth_usd = 100'000.0;
    intent.snapshot_mid_price = Price(50000.0);
    intent.stop_loss_price = Price(49500.0);     // -100 bps
    intent.take_profit_price = Price(50750.0);   // +150 bps → R:R = 1.5
    return intent;
}

FeatureSnapshot make_matching_snapshot(double mid = 50000.0,
                                        double spread_bps = 4.0,
                                        double depth_usd = 100'000.0) {
    FeatureSnapshot fs;
    fs.symbol = Symbol("BTCUSDT");
    fs.mid_price = Price(mid);
    fs.microstructure.mid_price = mid;
    fs.microstructure.spread_bps = spread_bps;
    fs.microstructure.spread_valid = true;
    fs.microstructure.liquidity_valid = true;
    fs.microstructure.bid_depth_5_notional = depth_usd / 2.0;
    fs.microstructure.ask_depth_5_notional = depth_usd / 2.0;
    return fs;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// FreshnessGate
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("FreshnessGate: свежий снимок проходит", "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    auto snap = make_matching_snapshot();
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;  // +100ms

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE(v.passed);
    REQUIRE(v.reason_code == "ok");
    REQUIRE(v.signal_age_ms == 100);
}

TEST_CASE("FreshnessGate: stale signal → блок", "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    auto snap = make_matching_snapshot();
    int64_t now = intent.signal_snapshot_ts_ns + 600'000'000LL;  // +600ms > 500ms

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "stale");
    REQUIRE(v.signal_age_ms == 600);
}

TEST_CASE("FreshnessGate: adverse BUY price drift → блок", "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();  // BUY, snap mid=50000
    // Цена ушла на +10 bps → BUY entry дороже → adverse
    auto snap = make_matching_snapshot(50050.0);
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "price_drift");
    REQUIRE(v.price_drift_bps > 8.0);
}

TEST_CASE("FreshnessGate: SELL adverse drift = цена ниже snapshot",
          "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.side = Side::Sell;
    intent.position_side = PositionSide::Short;
    intent.signal_intent = SignalIntent::ShortEntry;
    intent.stop_loss_price = Price(50500.0);
    intent.take_profit_price = Price(49250.0);

    // Цена упала на 10 bps → SELL вход дешевле → adverse
    auto snap = make_matching_snapshot(49950.0);
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "price_drift");
}

TEST_CASE("FreshnessGate: favorable drift пропускается",
          "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();  // BUY
    // Цена ушла на -10 bps → BUY дешевле → favorable, пропускаем
    auto snap = make_matching_snapshot(49950.0);
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE(v.passed);
    REQUIRE(v.reason_code == "ok");
    REQUIRE(v.price_drift_bps < 0.0);
}

TEST_CASE("FreshnessGate: расширение спреда → блок",
          "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    // spread 4 → 7 bps = +75%
    auto snap = make_matching_snapshot(50000.0, 7.0);
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "spread_widened");
}

TEST_CASE("FreshnessGate: исчезновение глубины → блок",
          "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    // depth 100k → 40k = 40% remaining < min 50%
    auto snap = make_matching_snapshot(50000.0, 4.0, 40'000.0);
    int64_t now = intent.signal_snapshot_ts_ns + 100'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "depth_thin");
}

TEST_CASE("FreshnessGate: closes пропускаются",
          "[gates][freshness][edge-31]") {
    FreshnessGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.trade_side = TradeSide::Close;
    intent.signal_intent = SignalIntent::LongExit;
    auto snap = make_matching_snapshot();
    int64_t now = intent.signal_snapshot_ts_ns + 5'000'000'000LL;  // +5s stale

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE(v.passed);  // Close обходит freshness gate
}

TEST_CASE("FreshnessGate: disabled → проходит без проверок",
          "[gates][freshness][edge-31]") {
    auto cfg = default_cfg();
    cfg.freshness_enabled = false;
    FreshnessGate gate(cfg);
    auto intent = make_entry_intent_with_snapshot();
    auto snap = make_matching_snapshot(60000.0, 100.0, 10.0);  // абсурдные условия
    int64_t now = intent.signal_snapshot_ts_ns + 60'000'000'000LL;

    auto v = gate.evaluate(intent, snap, now);
    REQUIRE(v.passed);
    REQUIRE(v.reason_code == "disabled");
}

// ─────────────────────────────────────────────────────────────────────────────
// NetRRGate
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("NetRRGate: R:R=1.5 BUY taker → net_rr выше порога",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    // gross: reward=150bps, risk=100bps, R:R=1.5
    // taker cost = 6*2 + 2*2 = 16 bps
    // net_reward = 150-16 = 134; net_rr = 134/100 = 1.34
    auto v = gate.evaluate(intent, 50000.0, 0.0, /*is_taker=*/true);
    REQUIRE(v.passed);
    REQUIRE_THAT(v.gross_rr, WithinAbs(1.5, 1e-3));
    REQUIRE_THAT(v.net_rr, WithinAbs(1.34, 0.05));
    REQUIRE(v.total_cost_bps > 0.0);
}

TEST_CASE("NetRRGate: тощий R:R 1.1 taker → net_rr < 0.5 → блок",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    // reward=110bps, risk=100bps → gross_rr=1.1
    // taker cost=16bps → net_reward=94, net_rr=0.94 — всё ещё выше 0.5
    // Сделаем уже: reward=20bps risk=100bps → gross=0.2, net = (20-16)/100 = 0.04
    intent.take_profit_price = Price(50100.0);   // +20 bps
    intent.stop_loss_price   = Price(49500.0);   // -100 bps
    auto v = gate.evaluate(intent, 50000.0, 0.0, /*is_taker=*/true);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "insufficient_net_rr");
    REQUIRE(v.net_rr < 0.5);
}

TEST_CASE("NetRRGate: maker fee → ниже cost → net_rr выше",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    // taker cost=16bps vs maker cost = 2*2 + 2*2 = 8bps
    auto v_taker = gate.evaluate(intent, 50000.0, 0.0, true);
    auto v_maker = gate.evaluate(intent, 50000.0, 0.0, false);
    REQUIRE(v_taker.passed);
    REQUIRE(v_maker.passed);
    REQUIRE(v_maker.net_rr > v_taker.net_rr);
    REQUIRE(v_maker.total_cost_bps < v_taker.total_cost_bps);
}

TEST_CASE("NetRRGate: SELL — корректная инверсия reward/risk",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.side = Side::Sell;
    intent.position_side = PositionSide::Short;
    intent.signal_intent = SignalIntent::ShortEntry;
    intent.stop_loss_price = Price(50500.0);     // SL выше на 100 bps
    intent.take_profit_price = Price(49250.0);   // TP ниже на 150 bps

    auto v = gate.evaluate(intent, 50000.0, 0.0, true);
    REQUIRE(v.passed);
    REQUIRE_THAT(v.gross_reward_bps, WithinAbs(150.0, 1.0));
    REQUIRE_THAT(v.gross_risk_bps, WithinAbs(100.0, 1.0));
    REQUIRE_THAT(v.gross_rr, WithinAbs(1.5, 1e-2));
}

TEST_CASE("NetRRGate: invalid TP/SL (на одной стороне) → блок",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.take_profit_price = Price(49900.0);  // TP ниже entry для BUY — ошибка
    auto v = gate.evaluate(intent, 50000.0, 0.0, true);
    REQUIRE_FALSE(v.passed);
    REQUIRE(v.reason_code == "invalid_tp_sl");
}

TEST_CASE("NetRRGate: missing TP/SL → no_tp_sl (Phase 1 fallback)",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.take_profit_price.reset();
    intent.stop_loss_price.reset();

    auto v = gate.evaluate(intent, 50000.0, 0.0, true);
    REQUIRE(v.passed);                        // fallback: проходим
    REQUIRE(v.reason_code == "no_tp_sl");
}

TEST_CASE("NetRRGate: closes пропускаются",
          "[gates][netrr][edge-31]") {
    NetRRGate gate(default_cfg());
    auto intent = make_entry_intent_with_snapshot();
    intent.trade_side = TradeSide::Close;
    auto v = gate.evaluate(intent, 50000.0, 0.0, true);
    REQUIRE(v.passed);
    REQUIRE(v.reason_code == "ok_close_skip");
}

TEST_CASE("NetRRGate: funding cost учитывается для long при positive rate",
          "[gates][netrr][edge-31]") {
    auto cfg = default_cfg();
    cfg.include_funding_cost = true;
    cfg.assumed_hold_minutes = 480.0;  // полные 8h = funding paid once
    NetRRGate gate(cfg);
    auto intent = make_entry_intent_with_snapshot();  // BUY

    // funding=0.001 (10 bps за 8h)
    auto v = gate.evaluate(intent, 50000.0, 0.001, true);
    REQUIRE(v.passed);
    // total_cost = 16 fee+slip + 10 funding = 26 bps
    REQUIRE_THAT(v.total_cost_bps, WithinAbs(26.0, 1.0));
}

TEST_CASE("NetRRGate: disabled → всегда проходит",
          "[gates][netrr][edge-31]") {
    auto cfg = default_cfg();
    cfg.net_rr_enabled = false;
    NetRRGate gate(cfg);
    auto intent = make_entry_intent_with_snapshot();
    // Заведомо токсичный setup — reward=10bps, risk=1000bps
    intent.take_profit_price = Price(50050.0);
    intent.stop_loss_price   = Price(45000.0);
    auto v = gate.evaluate(intent, 50000.0, 0.0, true);
    REQUIRE(v.passed);
    REQUIRE(v.reason_code == "disabled");
}
