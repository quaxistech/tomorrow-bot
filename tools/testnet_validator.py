#!/usr/bin/env python3
"""
Bitget API connectivity & auth validator.

Производит ТОЛЬКО read-only запросы. Не размещает ордера.
Цель: определить тип аккаунта (demo / mainnet), подтвердить:
  * REST signing correctness
  * timestamp drift
  * account hold-mode (hedge vs one-way)
  * margin mode
  * leverage
  * USDT баланс
  * наличие открытых позиций

Все ответы Bitget редактируются (api keys не появляются в выводе).

Usage:
    python3 tools/testnet_validator.py [--with-paptrading]

Если --with-paptrading указан, добавляется header `paptrading: 1`.
Bitget документация: paptrading mode doc / Demo Trading endpoint.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.parse
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import requests

SECRETS_PATH = Path("/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env")
BASE_URL = "https://api.bitget.com"


@dataclass
class Creds:
    api_key: str
    api_secret: str
    passphrase: str


def load_secrets(path: Path) -> Creds:
    out: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip().strip('"').strip("'")
        out[k.strip()] = v
    try:
        return Creds(out["BITGET_API_KEY"], out["BITGET_API_SECRET"], out["BITGET_PASSPHRASE"])
    except KeyError as e:
        raise SystemExit(f"missing secret: {e}")


def redact(s: str, keep_head: int = 2, keep_tail: int = 2) -> str:
    if len(s) <= keep_head + keep_tail:
        return "*" * len(s)
    return s[:keep_head] + "*" * (len(s) - keep_head - keep_tail) + s[-keep_tail:]


def sign_request(secret: str, ts_ms: int, method: str, request_path: str,
                 body: str = "") -> str:
    msg = f"{ts_ms}{method.upper()}{request_path}{body}"
    digest = hmac.new(secret.encode(), msg.encode(), hashlib.sha256).digest()
    return base64.b64encode(digest).decode()


def authed_get(creds: Creds, path: str, query: Optional[dict] = None,
               paptrading: bool = False) -> requests.Response:
    full_path = path
    if query:
        full_path = path + "?" + urllib.parse.urlencode(sorted(query.items()))
    ts_ms = int(time.time() * 1000)
    sig = sign_request(creds.api_secret, ts_ms, "GET", full_path, "")
    headers = {
        "ACCESS-KEY": creds.api_key,
        "ACCESS-SIGN": sig,
        "ACCESS-TIMESTAMP": str(ts_ms),
        "ACCESS-PASSPHRASE": creds.passphrase,
        "Content-Type": "application/json",
        "locale": "en-US",
    }
    if paptrading:
        headers["paptrading"] = "1"
    return requests.get(BASE_URL + full_path, headers=headers, timeout=10)


def section(title: str) -> None:
    print(f"\n=== {title} ===")


def dump_response(resp: requests.Response, label: str) -> dict:
    section(label)
    print(f"HTTP {resp.status_code}")
    try:
        data = resp.json()
    except Exception:
        print(f"raw body: {resp.text[:500]}")
        return {}
    # Pretty-print the JSON; we already redacted secrets in headers, body is exchange data.
    print(json.dumps(data, indent=2, ensure_ascii=False)[:2000])
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--with-paptrading", action="store_true",
                        help="Send `paptrading: 1` header (Bitget demo trading)")
    args = parser.parse_args()

    if not SECRETS_PATH.exists():
        print(f"[FATAL] secrets file not found: {SECRETS_PATH}", file=sys.stderr)
        return 1

    creds = load_secrets(SECRETS_PATH)
    print(f"[INFO] api_key   = {redact(creds.api_key, 4, 4)} (len={len(creds.api_key)})")
    print(f"[INFO] passphrase= {redact(creds.passphrase, 1, 1)} (len={len(creds.passphrase)})")
    print(f"[INFO] paptrading_header = {args.with_paptrading}")

    # ============================================================
    # 1) Public time — connectivity check
    # ============================================================
    section("1) Public time (no auth)")
    r = requests.get(BASE_URL + "/api/v2/public/time", timeout=10)
    print(f"HTTP {r.status_code}")
    print(json.dumps(r.json(), indent=2))
    server_ms = int(r.json()["data"]["serverTime"])
    local_ms = int(time.time() * 1000)
    drift = local_ms - server_ms
    print(f"[INFO] clock_drift_ms (local - server) = {drift}")

    # ============================================================
    # 2) Account info — main test for credential validity
    # ============================================================
    dump_response(
        authed_get(creds, "/api/v2/mix/account/accounts",
                   query={"productType": "USDT-FUTURES"},
                   paptrading=args.with_paptrading),
        "2) Mix accounts (USDT-FUTURES)"
    )

    # ============================================================
    # 3) Position-mode (hedge/oneway)
    # ============================================================
    dump_response(
        authed_get(creds, "/api/v2/mix/account/setting",
                   query={"productType": "USDT-FUTURES"},
                   paptrading=args.with_paptrading),
        "3) Account setting (position-mode + cross/iso)"
    )

    # ============================================================
    # 4) Open positions
    # ============================================================
    dump_response(
        authed_get(creds, "/api/v2/mix/position/all-position",
                   query={"productType": "USDT-FUTURES",
                          "marginCoin": "USDT"},
                   paptrading=args.with_paptrading),
        "4) Open positions"
    )

    # ============================================================
    # 5) Open orders
    # ============================================================
    dump_response(
        authed_get(creds, "/api/v2/mix/order/orders-pending",
                   query={"productType": "USDT-FUTURES"},
                   paptrading=args.with_paptrading),
        "5) Open orders"
    )

    # ============================================================
    # 6) Single-symbol detail (BTCUSDT) — verify symbol is tradable
    # ============================================================
    dump_response(
        authed_get(creds, "/api/v2/mix/account/account",
                   query={"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
                          "marginCoin": "USDT"},
                   paptrading=args.with_paptrading),
        "6) Per-symbol account (BTCUSDT)"
    )

    # ============================================================
    # 7) Public exchange info — verify symbol metadata (precision, min_notional)
    # ============================================================
    section("7) Public contract info (BTCUSDT)")
    r = requests.get(
        f"{BASE_URL}/api/v2/mix/market/contracts",
        params={"productType": "USDT-FUTURES", "symbol": "BTCUSDT"},
        timeout=10)
    print(f"HTTP {r.status_code}")
    j = r.json()
    if j.get("data"):
        # Print only relevant fields, response is huge.
        d = j["data"][0] if isinstance(j["data"], list) else j["data"]
        relevant = {k: d[k] for k in [
            "symbol", "baseCoin", "quoteCoin", "minTradeUSDT", "minTradeNum",
            "pricePlace", "volumePlace", "sizeMultiplier", "feeRateUpRatio",
            "openCostUpRatio", "supportMarginCoins", "symbolType",
            "symbolStatus", "buyLimitPriceRatio", "sellLimitPriceRatio"
        ] if k in d}
        print(json.dumps(relevant, indent=2))

    # ============================================================
    # 8) Public funding rate
    # ============================================================
    section("8) Public funding rate (BTCUSDT)")
    r = requests.get(
        f"{BASE_URL}/api/v2/mix/market/current-fund-rate",
        params={"productType": "USDT-FUTURES", "symbol": "BTCUSDT"},
        timeout=10)
    print(f"HTTP {r.status_code}")
    print(json.dumps(r.json(), indent=2))

    return 0


if __name__ == "__main__":
    sys.exit(main())
