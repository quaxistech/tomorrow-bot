#!/usr/bin/env python3
"""Trade journal analyzer — extracts trades from run logs and computes expectancy.

Reads logs/run*.log JSONL output, joins open/close events, aggregates by
strategy/symbol/setup_type, computes WR/PF/expectancy/avg_win/avg_loss.

Output: structured stats per bucket, flags buckets with negative expectancy.
"""
import json
import glob
import sys
from collections import defaultdict
from pathlib import Path
from typing import Optional


def load_trades_from_log(path: str):
    """Extract trades from a run log. Returns list of dicts with open+close info."""
    opens = {}   # order_id -> open event
    closes = []  # list of (order_id, close_event)
    setups = {}  # order_id -> setup info (latest seen before open)
    pending_setup = None
    last_market = {}  # symbol -> latest market snapshot

    try:
        with open(path) as f:
            for line in f:
                try:
                    e = json.loads(line)
                except Exception:
                    continue
                comp = e.get("component", "")
                msg = e.get("message", "")
                fields = e.get("fields", {}) or {}
                sym = fields.get("symbol", "")

                # Track latest market snapshot per symbol
                if msg == "Статус рынка" and sym:
                    last_market[sym] = fields

                # Track setup info as latest before open
                if msg == "Сетап обнаружен":
                    pending_setup = fields

                if msg == "Вход сгенерирован":
                    if pending_setup:
                        pending_setup = {**pending_setup, **fields}

                # Open position
                if msg == "Открыта позиция":
                    oid_key = (sym, fields.get("side"), fields.get("entry_price"))
                    opens[oid_key] = {
                        "ts": e.get("timestamp"),
                        "symbol": sym,
                        "side": fields.get("side"),
                        "entry_price": float(fields.get("entry_price", 0) or 0),
                        "size": float(fields.get("size", 0) or 0),
                        "setup": pending_setup or {},
                        "market_at_entry": dict(last_market.get(sym, {})),
                    }

                # Close events
                if msg in ("Позиция полностью закрыта через reduce (hedge)",
                          "Позиция полностью закрыта"):
                    closes.append({
                        "ts": e.get("timestamp"),
                        "symbol": sym,
                        "side": fields.get("side"),
                        "close_price": float(fields.get("close_price", 0) or 0),
                        "realized_pnl": float(fields.get("realized_pnl", 0) or 0),
                        "market_at_exit": dict(last_market.get(sym, {})),
                    })

                # Exit reason
                if msg in ("CONTINUATION_EXIT СРАБОТАЛ — принудительное закрытие",
                          "СТОП-ЛОСС СРАБОТАЛ — принудительное закрытие"):
                    if closes:
                        closes[-1]["exit_reason_full"] = fields.get("reason", "")
                        closes[-1]["exit_signal"] = fields.get("signal", "")
                        closes[-1]["urgency"] = float(fields.get("urgency", 0) or 0)
                        closes[-1]["max_adverse_excursion"] = fields.get("max_adverse_excursion_bps", "")
    except FileNotFoundError:
        return []

    # Pair opens and closes by symbol+side (long position closes with side=long, short with side=short)
    side_map = {"Buy": "long", "Sell": "short", "long": "long", "short": "short"}
    trades = []
    open_list = list(opens.values())
    for cl in closes:
        # Find earliest open matching symbol + side
        match_idx = None
        for i, op in enumerate(open_list):
            if op.get("matched"):
                continue
            if op["symbol"] != cl["symbol"]:
                continue
            op_side = side_map.get(op["side"], op["side"])
            cl_side = side_map.get(cl["side"], cl["side"])
            if op_side != cl_side:
                continue
            match_idx = i
            break
        if match_idx is not None:
            op = open_list[match_idx]
            op["matched"] = True
            trade = {**op, **{
                "close_ts": cl["ts"],
                "close_price": cl["close_price"],
                "realized_pnl": cl["realized_pnl"],
                "exit_reason": cl.get("exit_reason_full", ""),
                "urgency": cl.get("urgency", 0),
                "market_at_exit": cl.get("market_at_exit", {}),
            }}
            trades.append(trade)
    return trades


def estimate_cost(trade, fee_bps=6.0, slippage_bps=3.0):
    """Estimate round-trip cost in USDT. taker 6bps × 2 + spread/slippage ~5bps × 2."""
    notional = trade["entry_price"] * trade["size"]
    return notional * (fee_bps * 2 + slippage_bps * 2) / 10000.0


def bucket_stats(trades, key_fn):
    buckets = defaultdict(list)
    for t in trades:
        key = key_fn(t)
        buckets[key].append(t)
    out = {}
    for key, ts in buckets.items():
        n = len(ts)
        pnls = [t["realized_pnl"] for t in ts]
        costs = [estimate_cost(t) for t in ts]
        nets = [p - c for p, c in zip(pnls, costs)]
        wins = [n_ for n_ in nets if n_ > 0]
        losses = [n_ for n_ in nets if n_ <= 0]
        wr = len(wins) / n if n else 0
        avg_win = sum(wins) / len(wins) if wins else 0
        avg_loss = sum(losses) / len(losses) if losses else 0
        gross_win = sum(wins) if wins else 0
        gross_loss = -sum(losses) if losses else 0
        pf = gross_win / gross_loss if gross_loss > 0 else float("inf") if gross_win > 0 else 0
        expectancy = sum(nets) / n if n else 0
        out[key] = {
            "n": n, "wr": wr,
            "avg_win": avg_win, "avg_loss": avg_loss,
            "pf": pf, "expectancy": expectancy,
            "gross_pnl": sum(pnls), "gross_cost": sum(costs),
            "net_pnl": sum(nets),
        }
    return out


def fmt(x, fmt_spec=".4f"):
    if isinstance(x, float) and x == float("inf"):
        return "inf"
    return format(x, fmt_spec)


def main():
    log_dir = Path("logs")
    all_trades = []
    for log_file in sorted(log_dir.glob("run*.log")):
        if log_file.stat().st_size < 1000:
            continue
        ts = load_trades_from_log(str(log_file))
        for t in ts:
            t["run"] = log_file.stem
        all_trades.extend(ts)

    print(f"\n=== TOTAL TRADES EXTRACTED: {len(all_trades)} ===\n")
    if not all_trades:
        return

    # Overall stats
    overall = bucket_stats(all_trades, lambda t: "TOTAL")
    print("=== OVERALL ===")
    for k, s in overall.items():
        print(f"  n={s['n']} WR={s['wr']*100:.0f}% "
              f"avg_win=${s['avg_win']:.4f} avg_loss=${s['avg_loss']:.4f} "
              f"PF={fmt(s['pf'], '.2f')} expectancy=${s['expectancy']:.4f} "
              f"net=${s['net_pnl']:.4f} costs=${s['gross_cost']:.4f}")
    print()

    # By symbol
    print("=== BY SYMBOL ===")
    by_sym = bucket_stats(all_trades, lambda t: t["symbol"])
    for k in sorted(by_sym, key=lambda x: -by_sym[x]["net_pnl"]):
        s = by_sym[k]
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:14s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"PF={fmt(s['pf'], '.2f'):>5s} "
              f"E=${s['expectancy']:+.4f} net=${s['net_pnl']:+.4f}")
    print()

    # By setup type
    print("=== BY SETUP TYPE ===")
    by_setup = bucket_stats(all_trades, lambda t: t.get("setup", {}).get("type", "?"))
    for k in sorted(by_setup, key=lambda x: -by_setup[x]["net_pnl"]):
        s = by_setup[k]
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:30s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"PF={fmt(s['pf'], '.2f'):>5s} "
              f"E=${s['expectancy']:+.4f} net=${s['net_pnl']:+.4f}")
    print()

    # By side
    print("=== BY SIDE ===")
    by_side = bucket_stats(all_trades, lambda t: t["side"])
    for k, s in by_side.items():
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {str(k):10s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"PF={fmt(s['pf'], '.2f'):>5s} "
              f"E=${s['expectancy']:+.4f} net=${s['net_pnl']:+.4f}")
    print()

    # By exit reason
    print("=== BY EXIT REASON ===")
    def exit_cat(t):
        r = t.get("exit_reason", "")
        if "trailing stop" in r.lower(): return "trailing_stop"
        if "edge_decay" in r: return "edge_decay"
        if "stop hit" in r.lower(): return "hard_sl"
        if "Price loss" in r: return "hard_sl_pct"
        if "Funding" in r: return "funding_carry"
        if "Regime transition" in r: return "regime_transition"
        if "Continuation value" in r: return "continuation_exit"
        return r[:40] if r else "unknown"
    by_exit = bucket_stats(all_trades, exit_cat)
    for k in sorted(by_exit, key=lambda x: -by_exit[x]["net_pnl"]):
        s = by_exit[k]
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:25s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"PF={fmt(s['pf'], '.2f'):>5s} "
              f"E=${s['expectancy']:+.4f} net=${s['net_pnl']:+.4f}")
    print()

    # By hold time bucket
    print("=== BY HOLD TIME ===")
    import datetime
    def hold_bucket(t):
        try:
            t1 = datetime.datetime.fromisoformat(t["ts"].replace("Z", "+00:00"))
            t2 = datetime.datetime.fromisoformat(t["close_ts"].replace("Z", "+00:00"))
            sec = (t2 - t1).total_seconds()
        except Exception:
            return "?"
        if sec < 60: return "<1min"
        if sec < 180: return "1-3min"
        if sec < 600: return "3-10min"
        if sec < 1800: return "10-30min"
        return ">30min"
    by_hold = bucket_stats(all_trades, hold_bucket)
    for k in ["<1min", "1-3min", "3-10min", "10-30min", ">30min", "?"]:
        if k not in by_hold: continue
        s = by_hold[k]
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:10s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"PF={fmt(s['pf'], '.2f'):>5s} "
              f"E=${s['expectancy']:+.4f}")
    print()

    # By regime at entry
    print("=== BY REGIME (at entry) ===")
    by_reg = bucket_stats(all_trades, lambda t: t.get("market_at_entry", {}).get("regime", "?"))
    for k, s in sorted(by_reg.items(), key=lambda x: -x[1]["net_pnl"]):
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:15s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"E=${s['expectancy']:+.4f}")
    print()

    # By world state at entry
    print("=== BY WORLD STATE (at entry) ===")
    by_ws = bucket_stats(all_trades, lambda t: t.get("market_at_entry", {}).get("world", "?"))
    for k, s in sorted(by_ws.items(), key=lambda x: -x[1]["net_pnl"]):
        marker = "✓" if s["expectancy"] > 0 else "✗"
        print(f"  {marker} {k:30s} n={s['n']:3d} WR={s['wr']*100:3.0f}% "
              f"E=${s['expectancy']:+.4f}")
    print()

    # Auto-disable candidates
    print("=== AUTO-DISABLE CANDIDATES (n>=3 AND E<0) ===")
    found_disable = False
    for label, buckets in [("symbol", by_sym), ("setup", by_setup),
                           ("side", by_side), ("exit", by_exit),
                           ("regime", by_reg), ("world", by_ws)]:
        for k, s in buckets.items():
            if s["n"] >= 3 and s["expectancy"] < 0:
                print(f"  DISABLE {label}={k}: n={s['n']} E=${s['expectancy']:+.4f}")
                found_disable = True
    if not found_disable:
        print("  (none — insufficient data or all positive)")
    print()

    # Sample trade detail
    print("=== SAMPLE TRADES (latest 5) ===")
    for t in all_trades[-5:]:
        notional = t["entry_price"] * t["size"]
        cost = estimate_cost(t)
        net = t["realized_pnl"] - cost
        hold = ""
        try:
            import datetime
            t1 = datetime.datetime.fromisoformat(t["ts"].replace("Z", "+00:00"))
            t2 = datetime.datetime.fromisoformat(t["close_ts"].replace("Z", "+00:00"))
            hold = f"{(t2 - t1).total_seconds():.0f}s"
        except Exception:
            hold = "?"
        setup_t = t.get("setup", {}).get("type", "?")
        print(f"  {t['symbol']:10s} {str(t['side']):4s} {hold:>6s} "
              f"setup={setup_t:25s} "
              f"realized=${t['realized_pnl']:+.4f} cost=${cost:.4f} net=${net:+.4f}")


if __name__ == "__main__":
    main()
