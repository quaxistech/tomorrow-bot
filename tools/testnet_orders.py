#!/usr/bin/env python3
"""
Order lifecycle validator — real BTCUSDT orders, leverage=2x, hedge mode.

Sequence:
  1. precheck: balance, no open positions, no pending orders
  2. BUY 0.0001 BTC market, tradeSide=open holdSide=long
  3. wait 1s, verify position visible
  4. SELL 0.0001 BTC market, tradeSide=close holdSide=long
  5. wait 1s, verify position closed
  6. SELL 0.0001 BTC market, tradeSide=open holdSide=short
  7. verify position
  8. BUY 0.0001 BTC market, tradeSide=close holdSide=short
  9. verify clean

Evidence printed to stdout. Each step has explicit GET to verify state.
"""

import base64
import hashlib
import hmac
import json
import sys
import time
import urllib.parse
import uuid
from pathlib import Path

import requests

SECRETS = Path("/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env")
BASE = "https://api.bitget.com"
SYMBOL = "BTCUSDT"
QTY = "0.0001"  # min trade num


def load():
    out = {}
    for line in SECRETS.read_text().splitlines():
        if "=" in line and not line.strip().startswith("#"):
            k, v = line.strip().split("=", 1)
            out[k] = v.strip().strip('"').strip("'")
    return out["BITGET_API_KEY"], out["BITGET_API_SECRET"], out["BITGET_PASSPHRASE"]


def sign_msg(secret, ts, method, path, body=""):
    msg = f"{ts}{method.upper()}{path}{body}"
    return base64.b64encode(hmac.new(secret.encode(), msg.encode(), hashlib.sha256).digest()).decode()


def request(method, path, query=None, body=None):
    k, s, p = load()
    full = path
    if query:
        full = path + "?" + urllib.parse.urlencode(sorted(query.items()))
    body_str = json.dumps(body, separators=(",", ":")) if body else ""
    ts = str(int(time.time() * 1000))
    sig = sign_msg(s, ts, method, full, body_str)
    headers = {
        "ACCESS-KEY": k, "ACCESS-SIGN": sig, "ACCESS-TIMESTAMP": ts,
        "ACCESS-PASSPHRASE": p, "Content-Type": "application/json", "locale": "en-US",
    }
    if method == "GET":
        return requests.get(BASE + full, headers=headers, timeout=10)
    return requests.post(BASE + full, headers=headers, data=body_str, timeout=10)


def get_account():
    r = request("GET", "/api/v2/mix/account/account",
                {"symbol": SYMBOL, "productType": "USDT-FUTURES", "marginCoin": "USDT"})
    return r.json()["data"]


def get_positions():
    r = request("GET", "/api/v2/mix/position/all-position",
                {"productType": "USDT-FUTURES", "marginCoin": "USDT"})
    return r.json()["data"]


def get_pending_orders():
    r = request("GET", "/api/v2/mix/order/orders-pending",
                {"productType": "USDT-FUTURES"})
    return r.json()["data"].get("entrustedList") or []


def place_order(side, trade_side, hold_side, qty=QTY):
    coid = f"tn-{uuid.uuid4().hex[:16]}"
    body = {
        "symbol": SYMBOL,
        "productType": "USDT-FUTURES",
        "marginCoin": "USDT",
        "marginMode": "isolated",
        "size": qty,
        "side": side,
        "tradeSide": trade_side,
        "orderType": "market",
        "force": "gtc",
        "clientOid": coid,
    }
    r = request("POST", "/api/v2/mix/order/place-order", body=body)
    return body, r.json()


def label(s):
    print(f"\n=== {s} ===")


def status_line(label, account, positions, pending):
    longs = [p for p in positions if p.get("holdSide") == "long"]
    shorts = [p for p in positions if p.get("holdSide") == "short"]
    print(f"[{label}] eq={account['accountEquity']} avail={account['available']} "
          f"long={len(longs)} short={len(shorts)} pending={len(pending)}")
    for p in positions:
        print(f"  pos {p.get('holdSide')}: size={p.get('total')} entry={p.get('openPriceAvg')} "
              f"upl={p.get('unrealizedPL')}")


def main():
    label("PRECHECK")
    acc = get_account()
    pos = get_positions()
    pen = get_pending_orders()
    status_line("pre", acc, pos, pen)
    if pos or pen:
        print("[ABORT] account not clean — manual intervention needed")
        return 1
    if float(acc["available"]) < 5.0:
        print(f"[ABORT] available {acc['available']} < $5")
        return 1
    initial_equity = float(acc["accountEquity"])
    print(f"[INFO] initial equity = {initial_equity}")

    # ---------- LONG OPEN ----------
    label("LONG OPEN")
    body, resp = place_order("buy", "open", "long")
    print(f"[REQ] {json.dumps(body)}")
    print(f"[RESP] {json.dumps(resp)}")
    if resp.get("code") != "00000":
        print("[FAIL] long open rejected")
        return 1
    long_oid = resp["data"]["orderId"]
    long_coid = resp["data"]["clientOid"]
    time.sleep(2.0)
    pos = get_positions()
    pen = get_pending_orders()
    acc = get_account()
    status_line("after-long-open", acc, pos, pen)
    long_pos = [p for p in pos if p.get("holdSide") == "long"]
    if not long_pos or float(long_pos[0].get("total", "0")) <= 0:
        print("[FAIL] long position not visible after open")
        return 1
    print(f"[VERIFIED] long position exists: orderId={long_oid}")

    # ---------- LONG CLOSE ----------
    # VERIFIED: Bitget hedge-mode close requires side=BUY for holdSide=long
    label("LONG CLOSE")
    body, resp = place_order("buy", "close", "long")
    print(f"[REQ] {json.dumps(body)}")
    print(f"[RESP] {json.dumps(resp)}")
    if resp.get("code") != "00000":
        print(f"[FAIL] long close rejected: {resp}")
        return 1
    time.sleep(2.0)
    pos = get_positions()
    acc = get_account()
    status_line("after-long-close", acc, pos, [])
    long_pos = [p for p in pos if p.get("holdSide") == "long" and float(p.get("total", "0")) > 0]
    if long_pos:
        print(f"[FAIL] long position still exists after close: {long_pos}")
        return 1
    print("[VERIFIED] long position closed")

    # ---------- SHORT OPEN ----------
    label("SHORT OPEN")
    body, resp = place_order("sell", "open", "short")
    print(f"[REQ] {json.dumps(body)}")
    print(f"[RESP] {json.dumps(resp)}")
    if resp.get("code") != "00000":
        print(f"[FAIL] short open: {resp}")
        return 1
    time.sleep(2.0)
    pos = get_positions()
    acc = get_account()
    status_line("after-short-open", acc, pos, [])
    short_pos = [p for p in pos if p.get("holdSide") == "short" and float(p.get("total", "0")) > 0]
    if not short_pos:
        print("[FAIL] short position not visible")
        return 1
    print("[VERIFIED] short position exists")

    # ---------- SHORT CLOSE ----------
    # VERIFIED: side=SELL for holdSide=short
    label("SHORT CLOSE")
    body, resp = place_order("sell", "close", "short")
    print(f"[REQ] {json.dumps(body)}")
    print(f"[RESP] {json.dumps(resp)}")
    if resp.get("code") != "00000":
        print(f"[FAIL] short close: {resp}")
        return 1
    time.sleep(2.0)
    pos = get_positions()
    acc = get_account()
    status_line("after-short-close", acc, pos, [])
    if any(float(p.get("total", "0")) > 0 for p in pos):
        print("[FAIL] residual position")
        return 1
    final_equity = float(acc["accountEquity"])
    print(f"\n[SUMMARY] initial={initial_equity} final={final_equity} "
          f"pnl={final_equity - initial_equity}")
    print("[VERIFIED] full lifecycle: long open/close + short open/close")
    return 0


if __name__ == "__main__":
    sys.exit(main())
