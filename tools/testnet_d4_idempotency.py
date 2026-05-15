#!/usr/bin/env python3
"""
TN-3 verification: does Bitget return success on idempotent set_hold_mode /
set_margin_mode / set_leverage calls when value is already set?

If error → D4 fail-fast in TradingPipeline::start() falsely blocks pipeline
on properly-configured account.
"""

import base64
import hashlib
import hmac
import json
import sys
import time
import urllib.parse
from pathlib import Path

import requests

SECRETS = Path("/home/quaxis/projects/tomorrow-bot/.secrets/secrets.env")
BASE = "https://api.bitget.com"


def load():
    out = {}
    for line in SECRETS.read_text().splitlines():
        if "=" in line and not line.strip().startswith("#"):
            k, v = line.strip().split("=", 1)
            out[k] = v.strip().strip('"').strip("'")
    return out["BITGET_API_KEY"], out["BITGET_API_SECRET"], out["BITGET_PASSPHRASE"]


def sign(secret, ts, method, path, body=""):
    msg = f"{ts}{method.upper()}{path}{body}"
    return base64.b64encode(hmac.new(secret.encode(), msg.encode(), hashlib.sha256).digest()).decode()


def post(path, body):
    k, s, p = load()
    ts = str(int(time.time() * 1000))
    body_str = json.dumps(body, separators=(",", ":"))
    sig = sign(s, ts, "POST", path, body_str)
    headers = {
        "ACCESS-KEY": k, "ACCESS-SIGN": sig, "ACCESS-TIMESTAMP": ts,
        "ACCESS-PASSPHRASE": p, "Content-Type": "application/json",
        "locale": "en-US",
    }
    return requests.post(BASE + path, headers=headers, data=body_str, timeout=10)


def case(label, path, body):
    r = post(path, body)
    try:
        j = r.json()
    except:
        j = {"raw": r.text}
    print(f"\n[{label}] HTTP {r.status_code}")
    print(json.dumps(j, indent=2))
    return j


def main():
    print("=== TN-3 IDEMPOTENCY ===")

    # 1. set_hold_mode("hedge_mode") — already set per prior validation
    case("hold_mode hedge_mode (already set)",
         "/api/v2/mix/account/set-position-mode",
         {"productType": "USDT-FUTURES", "posMode": "hedge_mode"})

    # 2. set_margin_mode("isolated") — already set
    case("margin_mode isolated (already set)",
         "/api/v2/mix/account/set-margin-mode",
         {"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
          "marginCoin": "USDT", "marginMode": "isolated"})

    # 3. set_leverage long=2 (currently 7) — change to lower
    case("leverage long=2 (changing from 7)",
         "/api/v2/mix/account/set-leverage",
         {"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
          "marginCoin": "USDT", "leverage": "2", "holdSide": "long"})

    # 4. Repeat — leverage long=2 now equal — TEST IDEMPOTENCY
    case("leverage long=2 (idempotent — already 2)",
         "/api/v2/mix/account/set-leverage",
         {"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
          "marginCoin": "USDT", "leverage": "2", "holdSide": "long"})

    # 5. set_leverage short=2
    case("leverage short=2",
         "/api/v2/mix/account/set-leverage",
         {"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
          "marginCoin": "USDT", "leverage": "2", "holdSide": "short"})

    # 6. Repeat short=2
    case("leverage short=2 (idempotent)",
         "/api/v2/mix/account/set-leverage",
         {"symbol": "BTCUSDT", "productType": "USDT-FUTURES",
          "marginCoin": "USDT", "leverage": "2", "holdSide": "short"})

    return 0


if __name__ == "__main__":
    sys.exit(main())
