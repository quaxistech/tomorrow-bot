# Testnet Validation — Real Bitget Mainnet Evidence Log

**Date:** 2026-05-09
**Endpoint:** `https://api.bitget.com` (mainnet)
**Account:** real production account, micro-balance ($0.57 USDT)

> **CRITICAL:** API ключи в `.secrets/secrets.env` оказались **MAINNET production keys, не demo**. Balance — реальные $0.57 USDT, ниже Bitget min_notional ($5). **Order placement physically impossible** на этом аккаунте без funding или demo-ключей.
>
> Этот документ — evidence log для read-only validation против **реальной mainnet биржи**. Phase 2-4 (orders, soak) BLOCKED, см. § Verdict.

## Evidence sources

* [`tools/testnet_validator.py`](../tools/testnet_validator.py) — REST read-only auth + endpoints (Python). Reproducible.
* [`tools/testnet_ws_validator.py`](../tools/testnet_ws_validator.py) — WS public+private auth handshake.

Все запуски редактируют API key/secret/passphrase в выводе.

---

## Phase 1 — REST Connectivity & Authentication

### 1.1 Public time (connectivity baseline)

```
GET https://api.bitget.com/api/v2/public/time → HTTP 200
{"code":"00000","data":{"serverTime":"1778314324331"}}
clock_drift_ms (local - server) = 165
```

**VERIFIED:** Bitget reachable. Drift 165ms < recvWindow 5s. Bot's `check_clock_sync` startup gate (>2000ms abort) **passes**.

### 1.2 Authenticated GET `/api/v2/mix/account/accounts`

HMAC-SHA256 signing exactly как в `bitget_signing.cpp::make_auth_headers`:
`sign = base64( HMAC-SHA256(secret, ts_ms + "GET" + path + body) )`

```
HTTP 200 → {
  "code":"00000",
  "data":[{
    "marginCoin":"USDT",
    "available":"0.56842892",
    "accountEquity":"0.56842892",
    "usdtEquity":"0.568428928815",
    "assetMode":"single"
  }]
}
```

**VERIFIED:** signing scheme correct. Real account, $0.5684 USDT. `assetMode: single` (USDT-only margin).

### 1.3 Per-symbol account `/api/v2/mix/account/account` (BTCUSDT)

```
HTTP 200 → {
  "marginMode":"isolated",     ← matches bot's set_margin_mode()
  "posMode":"hedge_mode",       ← matches D4 fail-fast set_hold_mode("hedge_mode")
  "isolatedLongLever":7,        ← per-side leverage already set
  "isolatedShortLever":7,
  "crossedMarginLeverage":11
}
```

**VERIFIED:** exchange-side state matches bot's expectations exactly:
* `posMode: hedge_mode` ⟺ `set_hold_mode("USDT-FUTURES", "hedge_mode")` (D4)
* `marginMode: isolated` ⟺ `set_margin_mode(symbol, "isolated")`
* Per-side leverage (Long/Short separately) — confirms hedge-mode model

### 1.4 Open positions / orders

```
GET /api/v2/mix/position/all-position → "data": []
GET /api/v2/mix/order/orders-pending → "data": {"entrustedList": null}
```

**VERIFIED:** clean state. Reconciliation после bot start не найдёт orphans.

### 1.5 Public contract metadata `/api/v2/mix/market/contracts` (BTCUSDT)

```json
{
  "symbol":"BTCUSDT",
  "minTradeUSDT":"5",          // min notional
  "minTradeNum":"0.0001",       // min qty
  "pricePlace":"1",             // 1 decimal price
  "volumePlace":"4",            // 4 decimal qty
  "feeRateUpRatio":"0.005",
  "supportMarginCoins":["USDT"],
  "symbolType":"perpetual",
  "symbolStatus":"normal",
  "buyLimitPriceRatio":"0.05",
  "sellLimitPriceRatio":"0.05"
}
```

**VERIFIED:** bot's `format_quantity` (4 decimals) и `format_price` (1 decimal) precision rules match exchange truth. **Min notional $5 confirmed** — hard exchange requirement.

### 1.6 Public funding rate

```json
{
  "symbol":"BTCUSDT",
  "fundingRate":"-0.00001",
  "fundingRateInterval":"8",
  "nextUpdate":"1778342400000"
}
```

**VERIFIED:** bot's `FundingRateTracker` periodic update reads correct endpoint format.

### 1.7 Demo / paper-trading mode test

С header `paptrading: 1`:

```
HTTP 400 → {"code":"40099","msg":"exchange environment is incorrect"}
```

**VERIFIED:** keys MAINNET only. Code 40099 = key-environment mismatch. Demo trading требует separate API keys, созданные через Bitget UI для demo subaccount. **Provided keys are NOT testnet/demo.**

### 1.8 Endpoint mismatches

```
GET /api/v2/mix/account/setting → HTTP 404 (path не существует)
```

Эндпоинт был неправильным в моём validator'е. Bot не использует его (verified via grep). Не дефект bot'а.

---

## Phase 3-partial — WebSocket Validation

### 2.1 Public WS — `wss://ws.bitget.com/v2/ws/public`

```
SUBSCRIBE: {"op":"subscribe","args":[{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"}]}
ACK:       {"event":"subscribe","arg":{...}}

[RESULT] frames_received=20 in 10.6s (last_price=80371.4, ts_range_ms=8301)
```

**VERIFIED:**
* Subscription protocol совпадает с bot's `BitgetWsClient::do_subscribe_direct`.
* Frame rate 1.9 Hz consistent with Bitget ticker push.
* Real-world price: BTC = 80371.4 USDT (mainnet live).
* Frame format содержит `lastPr`, `ts` — matches `BitgetNormalizer::parse_ticker`.

### 2.2 Public WS — Reconnect cycles (3 cycles)

```
[CYCLE 1] connect→data→disconnect OK (1732ms)
[CYCLE 2] connect→data→disconnect OK (1757ms)
[CYCLE 3] connect→data→disconnect OK (1728ms)
```

**VERIFIED:** repeatable connect+subscribe+first-data ≈ 1.7s. Bot's `BitgetWsClient` reconnect path replays subscriptions identically.

**LIMITATION:** не verified под real network failure (TCP RST, DNS fail, mid-frame drop). Требуется toxiproxy.

### 2.3 Private WS — auth handshake

```
SEND: {"op":"login","args":[{
  "apiKey":"bg_0...934b",
  "passphrase":"q...h",
  "timestamp":"1778314531",
  "sign":"Do3f...NEM="
}]}
RECV: {"event":"login","code":0}
```

**VERIFIED:** bot's private WS login signing scheme (HMAC-SHA256 over `ts + "GET" + "/user/verify"`) **accepted by real Bitget**.

**LIMITATION:** не subscribed на private channels (orders/fill/account) — требуется capital для генерации событий.

---

## Phase 2 — Order Validation: BLOCKED

### Hard physical constraint

| Requirement | Value |
|-------------|-------|
| Account balance | $0.5684 USDT (mainnet) |
| Bitget min_notional (BTCUSDT) | $5.0 USDT |
| Max leverage available | 7× isolated (current setting) |

Even at 7×: notional ceiling = $0.5684 × 7 = **$3.98 < $5**. Bitget rejects all orders.

### NOT VERIFIED against real exchange

| Phase 2 capability | Status |
|--------------------|--------|
| Open long / Close long | NOT VERIFIED |
| Open short / Close short | NOT VERIFIED |
| Hedge long+short concurrent | NOT VERIFIED |
| `tradeSide=open`/`close` semantics | NOT VERIFIED |
| `reduceOnly`-via-`tradeSide=close` | NOT VERIFIED |
| Partial fill behavior | NOT VERIFIED |
| Cancel / cancel-replace | NOT VERIFIED |
| Stop-loss / take-profit (plan order) | NOT VERIFIED |
| Emergency cancel-all | NOT VERIFIED |
| WS fill event format | NOT VERIFIED |

### Phase 4 (6h soak) — BLOCKED

Bot at $0.57 balance не пройдёт `risk_engine`/`portfolio_allocator` — entry-orders блокируются. Public WS subscription run возможен, но даёт нулевой evidence по execution lifecycle.

---

## Findings — bot vs real Bitget

| ID | Finding | Status |
|----|---------|--------|
| TN-1 | Bitget code 40099 для `paptrading:1` на mainnet keys. Bot не использует paptrading. Documented. | INFO |
| TN-2 | `minTradeUSDT="5"` (string). Bot's `is_notional_valid` уже treats as double. | VERIFIED |
| TN-3 | `posMode:"hedge_mode"` already SET (предыдущие сессии bot'а). D4 fail-fast `set_hold_mode` будет idempotent. **Бот при start вызовет set_hold_mode → нужно проверить что Bitget возвращает success на already-set state, иначе D4 fail-fast будет ложно срабатывать.** | UNVERIFIED — высокий приоритет к проверке после funding |
| TN-4 | `assetMode:"single"` (USDT-only). Bot предполагает это. Matches. | VERIFIED |
| TN-5 | Real clock drift = 165ms на этой машине. Bot's threshold 2000ms — comfortable margin. | VERIFIED |

### TN-3 — потенциальная проблема для D4

В `TradingPipeline::start()` D4 fail-fast вызывает:
```cpp
if (!futures_submitter_->set_hold_mode(product_type, "hedge_mode")) {
    return false;  // pipeline отказывается стартовать
}
```

Если Bitget возвращает ошибку при попытке установить уже-установленный hold_mode (некоторые биржи делают так), bot **не сможет стартовать** даже на корректно настроенном аккаунте.

**Не verified в этой сессии** (требует funded account для запуска полного pipeline `start()`). Если verification на funded account покажет что Bitget возвращает error на repeated `set_hold_mode` — нужен fix в `BitgetFuturesOrderSubmitter::set_hold_mode`: трактовать "already set" как success.

---

## VERDICT

```
NOT READY for real-capital deployment.

Verified against real Bitget mainnet:
  ✓ REST signing (HMAC-SHA256 over ts+method+path)
  ✓ Authentication
  ✓ Clock drift handling
  ✓ Hedge mode + isolated margin already SET on account
  ✓ Per-side leverage model (long/short separately)
  ✓ Min notional $5, precision rules (volumePlace=4, pricePlace=1)
  ✓ Public WS subscribe + ticker frame format
  ✓ Public WS reconnect (3/3 cycles)
  ✓ Private WS auth handshake

NOT verified (all blocked by capital insufficiency):
  ✗ Order placement (any kind)
  ✗ tradeSide=open/close, reduceOnly
  ✗ Partial fills, cancel-replace, plan orders
  ✗ WS fill event format
  ✗ Real funding event accounting
  ✗ Reconnect storm under fault injection
  ✗ Rate-limit (429) handling
  ✗ TN-3: idempotent set_hold_mode (D4 fail-fast risk)

To unblock:
  1) Fund account to $20 USDT (4× min_notional buffer), OR
  2) Provide dedicated demo API keys (Bitget paper trading subaccount).
```

## Reproducible commands

```bash
# Phase 1 — REST read-only validation
python3 tools/testnet_validator.py
python3 tools/testnet_validator.py --with-paptrading

# Phase 3-partial — WS validation
python3 tools/testnet_ws_validator.py
```

Оба scripts standalone (deps: `requests`, `websockets`), не размещают orders, редактируют secrets.

## Operator action items

1. **Fund account to $20+ USDT** OR создать **dedicated demo API keys** через Bitget UI.
2. После funding/demo keys: re-run validators. Если все Phase 2 acceptance criteria выполнены — обновить этот документ; иначе документировать каждое расхождение с real Bitget behavior.
3. Решить вопрос **TN-3** (set_hold_mode idempotency) — критично для D4 fail-fast.

---

## ADDENDUM 2026-05-09 (account funded to $18.47)

### TN-3 RESOLVED — D4 idempotency verified

| Action | Result |
|--------|--------|
| `POST /api/v2/mix/account/set-position-mode posMode=hedge_mode` (already set) | `code:00000` |
| `POST /api/v2/mix/account/set-margin-mode marginMode=isolated` (already set) | `code:00000` |
| `POST /api/v2/mix/account/set-leverage long=2` (changing 7→2) | `code:00000` |
| `POST /api/v2/mix/account/set-leverage long=2` (idempotent repeat) | `code:00000` |
| `POST /api/v2/mix/account/set-leverage short=2` | `code:00000` |
| `POST /api/v2/mix/account/set-leverage short=2` (idempotent repeat) | `code:00000` |

**Bitget treats idempotent calls as success.** D4 fail-fast in `TradingPipeline::start()` is safe. TN-3 closed.

### BUG-EXCH-1 — CRITICAL: bot's close-side convention inverted

Empirical Bitget hedge-mode rules (verified against mainnet):

| Action | Required side | What bot was sending (BUGGY) |
|--------|---------------|------------------------------|
| Open LONG  | `side=buy`  | side=buy ✓ |
| Close LONG | `side=buy`  | **side=sell** ✗ — Bitget code:22002 "No position to close" |
| Open SHORT | `side=sell` | side=sell ✓ |
| Close SHORT| `side=sell` | **side=buy** ✗ — Bitget code:22002 |

**Convention:** В hedge_mode `side` ВСЕГДА совпадает с `holdSide`, независимо от `tradeSide`. Это противоположно one-way mode.

**Reproduction (mainnet):**
```
# OPEN LONG (works)
POST /api/v2/mix/order/place-order
{"side":"buy","tradeSide":"open","holdSide":"long",...}
→ code:00000

# CLOSE LONG with side=sell (bot's old behavior) — REJECTED
POST /api/v2/mix/order/place-order
{"side":"sell","tradeSide":"close","holdSide":"long",...}
→ code:22002 "No position to close"  ← position visible но Bitget rejects

# CLOSE LONG with side=buy — ACCEPTED
POST /api/v2/mix/order/place-order
{"side":"buy","tradeSide":"close","holdSide":"long",...}
→ code:00000  ← position closed
```

**Impact (pre-fix):**
* Bot could OPEN positions but **could not CLOSE them via normal close path**.
* Stop-loss / take-profit / strategy exits would all return 22002.
* Positions would accumulate, cash reserved indefinitely until manual close on Bitget UI.
* This is a **CAPITAL RISK** — bot opens but cannot exit.

**Why existing unit tests missed:**
* `tests/integration/bitget_order_mapping_test.cpp` checked OrderRecord fields (Buy/Sell at TradeIntent layer) but NOT actual JSON output of submitter.
* The buggy mapping was inside `BitgetFuturesOrderSubmitter::build_place_order_json` (line 392-399), private function not covered by tests.

**Fix:** [src/exchange/bitget/bitget_futures_order_submitter.cpp](../src/exchange/bitget/bitget_futures_order_submitter.cpp)
```cpp
// BEFORE (BUGGY):
if (Long) api_side = (open) ? "buy"  : "sell";   // close-long: sell ← wrong
else      api_side = (open) ? "sell" : "buy";    // close-short: buy ← wrong

// AFTER (verified against real Bitget):
if (Long) api_side = "buy";    // both open AND close — matches holdSide
else      api_side = "sell";
api_trade_side = (open) ? "open" : "close";
```

**Regression tests:** [tests/integration/bitget_order_mapping_test.cpp](../tests/integration/bitget_order_mapping_test.cpp)
* `BUG-EXCH-1: close LONG produces side=buy in JSON` (PASS)
* `BUG-EXCH-1: close SHORT produces side=sell in JSON` (PASS)
* `OrderMapping: open LONG produces side=buy` (PASS)
* `OrderMapping: open SHORT produces side=sell` (PASS)

The regression tests build the actual JSON via `submitter.build_place_order_json(order)` and verify with boost::json — this catches the bug at the JSON-output layer.

### Live lifecycle verified end-to-end (post-fix)

```
PRECHECK: equity=18.4068, available=18.4068, no positions, no orders
LONG OPEN  (0.0001 BTC @ ~$80840) → code:00000, orderId 1437085884766384129
LONG CLOSE (side=buy+close+long)  → code:00000
SHORT OPEN (side=sell+open+short) → code:00000, entry=80839.9
SHORT CLOSE(side=sell+close+short)→ code:00000
SUMMARY: initial=18.4068 final=18.3744 pnl=-0.032 (4 × 0.0006 fee = expected $0.020 + slippage)
```

### Updated VERIFIED matrix

| Capability | Status | Evidence |
|------------|--------|----------|
| D4 idempotency (set_hold_mode/margin_mode/leverage repeated) | ✅ VERIFIED | testnet_d4_idempotency.py |
| Open long market | ✅ VERIFIED | order 1437085884766384129 |
| Close long market | ✅ VERIFIED (post-BUG-EXCH-1 fix) | side=buy+close+long |
| Open short market | ✅ VERIFIED | side=sell+open+short |
| Close short market | ✅ VERIFIED | side=sell+close+short |
| Position state on Bitget matches local | ✅ VERIFIED | get_positions calls between each step |
| Margin reservation/release | ✅ VERIFIED | available drops from 18.40 to 14.36 on open, returns to 18.40 on close |
| Hedge mode (long+short concurrent) | ⏳ PENDING | Not yet tested |
| Partial fill | ⏳ PENDING |
| reduceOnly via tradeSide=close | ✅ VERIFIED (close path uses tradeSide=close) |
| Cancel pending limit | ⏳ PENDING |
| Stop-loss / take-profit (plan order) | ⏳ PENDING |
| WS fill event format | ⏳ PENDING |
| 6h+ live soak | ⏳ PENDING |

### Reproducible commands (live)

```bash
# D4 idempotency
python3 tools/testnet_d4_idempotency.py

# Full order lifecycle (places real orders, ~$0.03 PnL drag in fees)
python3 tools/testnet_orders.py
```
