# Protective Bracket Manager (edge-31 Phase 3)

**Файлы:** `protective_bracket_manager.{hpp,cpp}`
**Введено:** Phase 3 TPSL refactor (2026-05-16)

## Назначение

Owner of TP/SL bracket lifecycle для каждой открытой позиции. Гарантирует что bracket
существует на бирже (не только локально), верифицирует preset TPSL после fill,
ставит fallback standalone plan-orders при необходимости, поддерживает trailing
SL replacement, освобождает orphans при close.

## Lifecycle

```
on_position_opened(symbol, side, entry, qty, tp, sl)
  → register state (tp_source=PresetAttached, verified=false)

[periodic tick — каждые 1.5s в trading_pipeline]
verify_brackets(symbol, side)
  → after verify_grace_ms (3000ms default):
       query Bitget /api/v2/mix/order/orders-plan-pending
       match plan-ордера по symbol+side+trigger_price (tolerance 0.5%)
       record found order_ids в bracket state
  → если не нашли TP/SL после max_verify_attempts (6):
       fallback: place_standalone_plan() через submit_plan_order
  → если place fails N раз подряд: throttle (failed_backoff_ms = 60s)

[Phase 4 trailing — update_sl()]
update_sl(symbol, side, new_sl_price)
  → place новый plan-order ПЕРЕД отменой старого (always SL coverage)
  → cancel старый sl_order_id
  → update stored state

[on position close]
release(symbol, side)
  → cancel known plan-orders (TP + SL)
  → erase state

[on startup]
recover_from_exchange(open_positions)
  → for each open position: query plan-orders, populate bracket state с found IDs
cleanup_orphans_for_symbol(symbol, has_open_position)
  → cancel plan-orders для символа если нет открытой позиции (run90)
```

## API

```cpp
class ProtectiveBracketManager {
  void on_position_opened(symbol, ps, entry, qty, tp, sl);
  bool verify_brackets(symbol, ps);   // throttled, retry-aware
  bool update_sl(symbol, ps, new_sl);  // Phase 4 trailing entry
  bool release(symbol, ps);
  optional<BracketState> get_state(symbol, ps);
  vector<BracketState> list_active();
  int recover_from_exchange(open_positions);
  int cleanup_orphans_for_symbol(symbol, has_open_position);
};
```

## BracketState

```cpp
struct BracketState {
  Symbol symbol; PositionSide position_side;
  double position_size, entry_price, tp_price, sl_price;
  OrderId tp_order_id, sl_order_id;
  BracketSource tp_source, sl_source;  // None/PresetAttached/StandalonePlan
  int64_t opened_at_ns, last_verified_at_ns, throttle_until_ns;
  int verify_attempts, consecutive_fallback_failures;
  bool verified, released;
};
```

## Config

```cpp
struct ProtectiveBracketConfig {
  int64_t verify_grace_ms{3000};            // run90: Bitget indexing 2-3s
  int max_verify_attempts{6};
  int64_t verify_retry_interval_ms{1000};
  int max_consecutive_fallback_failures{3}; // anti-spam
  int64_t failed_backoff_ms{60'000};
};
```

## Bitget API

- **Verify**: `GET /api/v2/mix/order/orders-plan-pending?planType=profit_loss`
- **Fallback place**: `POST /api/v2/mix/order/place-plan-order` с `planType="normal_plan"` (run90 fix — было обязательное поле missing → 400172).
- **Cancel**: `POST /api/v2/mix/order/cancel-plan-order`

## Тесты

`tests/unit/pipeline/protective_bracket_manager_test.cpp` — 10 cases (на StubSubmitter + StubQuery).
