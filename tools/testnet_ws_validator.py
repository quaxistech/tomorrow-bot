#!/usr/bin/env python3
"""
Bitget WebSocket validator — public + private auth handshake.

Read-only: подписывается на ticker для BTCUSDT через public WS, для private —
выполняет login handshake (доказывает auth), затем отключается.
Не размещает ордера.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import sys
import time
from pathlib import Path
from typing import Optional

import websockets

SECRETS_PATH = Path("/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env")
PUBLIC_WS = "wss://ws.bitget.com/v2/ws/public"
PRIVATE_WS = "wss://ws.bitget.com/v2/ws/private"


def load_secrets() -> tuple[str, str, str]:
    out: dict[str, str] = {}
    for line in SECRETS_PATH.read_text().splitlines():
        line = line.strip()
        if "=" not in line or line.startswith("#"):
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip().strip('"').strip("'")
    return out["BITGET_API_KEY"], out["BITGET_API_SECRET"], out["BITGET_PASSPHRASE"]


def redact(s: str, keep: int = 4) -> str:
    if len(s) <= keep * 2:
        return "*" * len(s)
    return s[:keep] + "*" * (len(s) - keep * 2) + s[-keep:]


def sign_login(secret: str, ts_sec: int) -> str:
    msg = f"{ts_sec}GET/user/verify"
    digest = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).digest()
    return base64.b64encode(digest).decode()


# ============================================================
# Public WS — subscribe to BTCUSDT ticker, count frames for 10s
# ============================================================
async def public_ws_test() -> None:
    print("\n=== Public WS: subscribe BTCUSDT ticker, 10s sample ===")
    sub_msg = {
        "op": "subscribe",
        "args": [{"instType": "USDT-FUTURES", "channel": "ticker", "instId": "BTCUSDT"}]
    }
    frame_count = 0
    last_price: Optional[str] = None
    seq_first: Optional[int] = None
    seq_last: Optional[int] = None
    start = time.time()
    try:
        async with websockets.connect(PUBLIC_WS, ping_interval=20) as ws:
            await ws.send(json.dumps(sub_msg))
            print(f"[INFO] subscribed: {json.dumps(sub_msg)}")
            while time.time() - start < 10:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
                except asyncio.TimeoutError:
                    continue
                msg = json.loads(raw)
                if msg.get("event") == "subscribe":
                    print(f"[INFO] subscribe ACK: {raw[:200]}")
                    continue
                if "data" in msg:
                    frame_count += 1
                    arr = msg["data"]
                    if isinstance(arr, list) and arr:
                        last_price = arr[0].get("lastPr")
                        ts = int(arr[0].get("ts", "0"))
                        if seq_first is None:
                            seq_first = ts
                        seq_last = ts
        elapsed = time.time() - start
        if seq_first and seq_last:
            ts_range = seq_last - seq_first
        else:
            ts_range = 0
        print(f"[RESULT] frames_received={frame_count} in {elapsed:.1f}s "
              f"(last_price={last_price}, ts_range_ms={ts_range})")
    except Exception as e:
        print(f"[FAIL] {e}")


async def public_ws_reconnect_test() -> None:
    print("\n=== Public WS: connect → disconnect → reconnect (3 cycles) ===")
    for cycle in range(3):
        t0 = time.time()
        try:
            async with websockets.connect(PUBLIC_WS, ping_interval=20, close_timeout=5) as ws:
                await ws.send(json.dumps({
                    "op": "subscribe",
                    "args": [{"instType": "USDT-FUTURES", "channel": "ticker", "instId": "BTCUSDT"}]
                }))
                first_data = False
                while not first_data and time.time() - t0 < 5:
                    raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
                    msg = json.loads(raw)
                    if msg.get("data"):
                        first_data = True
            connect_ms = int((time.time() - t0) * 1000)
            print(f"[CYCLE {cycle+1}] connect→data→disconnect OK ({connect_ms}ms)")
        except Exception as e:
            print(f"[CYCLE {cycle+1}] FAIL: {e}")


async def private_ws_login_test() -> None:
    print("\n=== Private WS: auth handshake (no subscriptions, no orders) ===")
    api_key, api_secret, passphrase = load_secrets()
    ts = int(time.time())
    sig = sign_login(api_secret, ts)
    login_msg = {
        "op": "login",
        "args": [{
            "apiKey": api_key,
            "passphrase": passphrase,
            "timestamp": str(ts),
            "sign": sig,
        }]
    }
    safe_msg = json.loads(json.dumps(login_msg))
    safe_msg["args"][0]["apiKey"] = redact(api_key)
    safe_msg["args"][0]["passphrase"] = redact(passphrase, 1)
    safe_msg["args"][0]["sign"] = redact(sig)
    print(f"[INFO] login msg (redacted): {json.dumps(safe_msg)}")

    try:
        async with websockets.connect(PRIVATE_WS, ping_interval=20) as ws:
            await ws.send(json.dumps(login_msg))
            for _ in range(5):
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=5.0)
                except asyncio.TimeoutError:
                    print("[FAIL] no response from private WS within 5s")
                    return
                msg = json.loads(raw)
                if msg.get("event") in ("login", "error"):
                    print(f"[INFO] private WS auth response: {raw}")
                    if msg.get("event") == "login" and msg.get("code") == "0":
                        print("[VERIFIED] private WS authenticated successfully")
                    return
    except Exception as e:
        print(f"[FAIL] private WS error: {e}")


async def main() -> int:
    await public_ws_test()
    await public_ws_reconnect_test()
    await private_ws_login_test()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
