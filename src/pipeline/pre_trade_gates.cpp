#include "pipeline/pre_trade_gates.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace tb::pipeline {

// ─── FreshnessGate ────────────────────────────────────────────────────────────

FreshnessVerdict FreshnessGate::evaluate(const strategy::TradeIntent& intent,
                                          const features::FeatureSnapshot& current,
                                          int64_t now_ns) const {
    FreshnessVerdict v;

    if (!cfg_.freshness_enabled) {
        v.reason_code = "disabled";
        return v;
    }

    // Закрытия и reduce — не проверяем freshness: они должны исполниться
    // безусловно, даже если контекст изменился.
    if (intent.trade_side != TradeSide::Open) {
        v.reason_code = "ok_close_skip";
        return v;
    }

    // Если snapshot не заполнен (legacy intent или fallback path) — пропускаем
    // проверку, иначе всегда блокировали бы вход. Заполнение snapshot — обязанность
    // strategy::build_intent (см. Phase 1).
    if (intent.signal_snapshot_ts_ns <= 0 || intent.signal_snapshot_mid <= 0.0) {
        v.reason_code = "ok_no_snapshot";
        return v;
    }

    // 1. signal age
    const int64_t age_ns = std::max<int64_t>(0, now_ns - intent.signal_snapshot_ts_ns);
    v.signal_age_ms = age_ns / 1'000'000;
    if (v.signal_age_ms > cfg_.max_signal_age_ms) {
        v.passed = false;
        v.reason_code = "stale";
        v.reason_detail = std::format("signal_age={}ms > max={}ms",
                                       v.signal_age_ms, cfg_.max_signal_age_ms);
        return v;
    }

    // 2. price drift — в неблагоприятную сторону:
    //    BUY  → текущая цена выше snapshot mid (entry дороже, чем планировали)
    //    SELL → текущая цена ниже snapshot mid (entry дешевле, чем планировали)
    const double cur_mid = current.microstructure.mid_price;
    if (cur_mid > 0.0 && std::isfinite(cur_mid)) {
        double drift_bps = (cur_mid - intent.signal_snapshot_mid) /
                           intent.signal_snapshot_mid * 10000.0;
        // BUY adverse = positive drift, SELL adverse = negative drift.
        // Нормализуем: adverse_drift всегда positive.
        double adverse_bps = (intent.side == Side::Buy) ? drift_bps : -drift_bps;
        v.price_drift_bps = adverse_bps;
        if (adverse_bps > cfg_.max_adverse_price_drift_bps) {
            v.passed = false;
            v.reason_code = "price_drift";
            v.reason_detail = std::format("adverse_drift={:.2f}bps > max={:.2f}bps",
                                           adverse_bps, cfg_.max_adverse_price_drift_bps);
            return v;
        }
    }

    // 3. spread widening
    if (current.microstructure.spread_valid && intent.signal_snapshot_spread_bps > 0.0) {
        double cur_spread = current.microstructure.spread_bps;
        double widen_pct = (cur_spread - intent.signal_snapshot_spread_bps) /
                            intent.signal_snapshot_spread_bps * 100.0;
        v.spread_widen_pct = widen_pct;
        if (widen_pct > cfg_.max_spread_widen_pct) {
            v.passed = false;
            v.reason_code = "spread_widened";
            v.reason_detail = std::format(
                "spread {:.2f}→{:.2f}bps widen={:.0f}% > max={:.0f}%",
                intent.signal_snapshot_spread_bps, cur_spread,
                widen_pct, cfg_.max_spread_widen_pct);
            return v;
        }
    }

    // 4. depth thinning — остаток глубины в %
    if (current.microstructure.liquidity_valid && intent.signal_snapshot_depth_usd > 0.0) {
        double cur_depth = current.microstructure.bid_depth_5_notional
                          + current.microstructure.ask_depth_5_notional;
        double remain_pct = (cur_depth / intent.signal_snapshot_depth_usd) * 100.0;
        v.depth_remain_pct = remain_pct;
        if (remain_pct < cfg_.min_depth_remain_pct) {
            v.passed = false;
            v.reason_code = "depth_thin";
            v.reason_detail = std::format(
                "depth {:.0f}→{:.0f}USD remain={:.0f}% < min={:.0f}%",
                intent.signal_snapshot_depth_usd, cur_depth,
                remain_pct, cfg_.min_depth_remain_pct);
            return v;
        }
    }

    v.reason_code = "ok";
    return v;
}

// ─── NetRRGate ────────────────────────────────────────────────────────────────

NetRRVerdict NetRRGate::evaluate(const strategy::TradeIntent& intent,
                                  double current_mid,
                                  double funding_rate_8h,
                                  bool is_taker) const {
    NetRRVerdict v;

    if (!cfg_.net_rr_enabled) {
        v.reason_code = "disabled";
        return v;
    }

    // Закрытия не оцениваем — net-RR определяется для входа.
    if (intent.trade_side != TradeSide::Open) {
        v.reason_code = "ok_close_skip";
        return v;
    }

    if (!intent.take_profit_price.has_value() || !intent.stop_loss_price.has_value()) {
        v.reason_code = "no_tp_sl";
        v.reason_detail = "TP/SL не заданы — gate пропускает (Phase 1 fallback)";
        return v;
    }

    double entry = current_mid;
    if (entry <= 0.0 || !std::isfinite(entry)) {
        if (intent.snapshot_mid_price.has_value() && intent.snapshot_mid_price->get() > 0.0) {
            entry = intent.snapshot_mid_price->get();
        } else {
            v.passed = false;
            v.reason_code = "no_entry_price";
            v.reason_detail = "current_mid<=0 и snapshot_mid отсутствует";
            return v;
        }
    }

    double tp = intent.take_profit_price->get();
    double sl = intent.stop_loss_price->get();

    // Reward / Risk в bps от entry. Для BUY: reward = (TP - entry), risk = (entry - SL).
    // Для SELL: reward = (entry - TP), risk = (SL - entry).
    double reward_abs = 0.0;
    double risk_abs = 0.0;
    if (intent.side == Side::Buy) {
        reward_abs = tp - entry;
        risk_abs   = entry - sl;
    } else {
        reward_abs = entry - tp;
        risk_abs   = sl - entry;
    }

    if (risk_abs <= 0.0 || reward_abs <= 0.0) {
        v.passed = false;
        v.reason_code = "invalid_tp_sl";
        v.reason_detail = std::format(
            "side={} entry={:.6f} tp={:.6f} sl={:.6f} reward={:.6f} risk={:.6f}",
            intent.side == Side::Buy ? "BUY" : "SELL", entry, tp, sl, reward_abs, risk_abs);
        return v;
    }

    v.gross_reward_bps = reward_abs / entry * 10000.0;
    v.gross_risk_bps   = risk_abs / entry * 10000.0;
    v.gross_rr = v.gross_reward_bps / v.gross_risk_bps;

    // Total cost: round-trip fees (entry + exit) + slippage (entry + exit) + опционально funding.
    // На скальпинг-сетапах часто entry — taker (Aggressive style при urgency≥0.8), exit — taker
    // (close через market). Maker rebate возможен если PostOnly прошёл — берём пессимистично.
    double fee_per_leg_bps = is_taker ? cfg_.taker_fee_bps : cfg_.maker_fee_bps;
    double total_fee_bps = fee_per_leg_bps * 2.0;
    double total_slip_bps = cfg_.assumed_slippage_bps_per_leg * 2.0;

    double funding_bps = 0.0;
    if (cfg_.include_funding_cost && std::isfinite(funding_rate_8h)) {
        // funding_rate_8h applies каждые 8 часов. За hold X минут платится
        // (X / 480) * funding_rate_8h * 10000 bps.
        double hold_fraction_8h = std::clamp(cfg_.assumed_hold_minutes / 480.0, 0.0, 10.0);
        double funding_signed = funding_rate_8h * hold_fraction_8h * 10000.0;
        // Для long positive funding = плата (cost). Для short — наоборот.
        funding_bps = (intent.side == Side::Buy) ? funding_signed : -funding_signed;
        // Если negative — это rebate → уменьшает cost. Cap снизу 0 — gate
        // не должен выдавать "негативную стоимость" как бесплатный буст.
        funding_bps = std::max(funding_bps, 0.0);
    }

    v.total_cost_bps = total_fee_bps + total_slip_bps + funding_bps;
    v.net_reward_bps = v.gross_reward_bps - v.total_cost_bps;
    v.net_rr = (v.gross_risk_bps > 0.0) ? (v.net_reward_bps / v.gross_risk_bps) : 0.0;

    if (v.net_rr < cfg_.min_net_rr) {
        v.passed = false;
        v.reason_code = "insufficient_net_rr";
        v.reason_detail = std::format(
            "gross_rr={:.2f} cost={:.2f}bps net_rr={:.2f} < min={:.2f}",
            v.gross_rr, v.total_cost_bps, v.net_rr, cfg_.min_net_rr);
        return v;
    }

    v.reason_code = "ok";
    return v;
}

} // namespace tb::pipeline
