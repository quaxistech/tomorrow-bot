# Periodic Trailing SL

**Файлы:** `periodic_trailing_sl.{hpp,cpp}`
**Введено:** run87 (2026-05-17)

## Назначение

Каждые N секунд (default 3) пересчитывает trailing SL по Chandelier Exit для всех
открытых позиций. SL двигается **ТОЛЬКО в сторону прибыли** (monotonic).
Update идёт через `ProtectiveBracketManager.update_sl()` — cancel старого
plan-ордера + установка нового на бирже. Plan-ордер live on exchange, trigger атомарно.

## Lifecycle

```
on each pipeline tick:
  if has_open_position AND throttle_interval_passed:
    snap = build snapshot (entry, current, atr, supertrend_trend, cvd_div, liq_cascade)
    PeriodicTrailingSl.tick(snap)
      → check activation: profit_bps >= activation_min_profit_bps (20)
      → compute new_sl = Chandelier Exit:
           LONG:  new_sl = high_since_entry - atr × multiplier
           SHORT: new_sl = low_since_entry + atr × multiplier
      → adaptive multiplier (run94):
           supertrend против позиции → ×0.6 (быстрее exit)
           CVD divergence против → ×0.7
           liq cascade risk > 0.7 → ×0.5
      → CRITICAL clamp на breakeven+fees buffer (run89 OPGUSDT loss-zone bug fix):
           LONG:  new_sl = max(new_sl, entry × (1 + 0.0015))   // +15 bps buffer
           SHORT: new_sl = min(new_sl, entry × (1 - 0.0015))
      → monotonic check: new_sl выше старого (LONG) / ниже (SHORT)?
      → min move check: > 10 bps от текущего SL?
      → bracket_manager.update_sl(symbol, ps, new_sl)
```

## API

```cpp
struct TrailingPositionSnapshot {
  Symbol symbol; PositionSide position_side;
  double entry_price, current_price;
  double highest_since_entry, lowest_since_entry;
  double atr;
  // run94 contextual:
  int supertrend_trend;
  bool cvd_bullish_div, cvd_bearish_div;
  double liq_cascade_risk;
};

class PeriodicTrailingSl {
  PeriodicTrailingSl(bracket_manager, logger, clock, cfg);
  int tick(const TrailingPositionSnapshot& snap);  // returns 1 if applied
  void force_recheck();
};
```

## Config

```cpp
struct PeriodicTrailingConfig {
  int64_t min_interval_ms{3000};            // throttle
  double atr_trail_multiplier{0.9};         // Chandelier base
  double min_sl_move_bps{10.0};             // min |Δsl| / sl × 10000 to apply
  double activation_min_profit_bps{20.0};   // run95: 30 → 20 (10/11 losses без trail)
  bool enabled{true};
};
```

## Breakeven Clamp (run89 critical fix)

Без clamp Chandelier ставил SL в **loss-zone** (например для SHORT entry 0.25225,
ATR×0.9 от current → new_sl = 0.25294 = ВЫШЕ entry → trigger = loss).

Clamp гарантирует:
- LONG: SL ≥ entry × (1 + 0.0015)
- SHORT: SL ≤ entry × (1 - 0.0015)

15 bps buffer = round-trip fees (12 bps) + safety margin. Гарантирует close at SL = small profit.

## Метрика

- `updates_applied` — сколько успешных move
- `updates_skipped_small` — move < min_sl_move_bps
- `updates_skipped_not_profitable` — profit < activation_min_profit_bps

## Интеграция в trading_pipeline.cpp

```cpp
if (trailing_sl_ && bracket_manager_ && has_open_position()) {
  TrailingPositionSnapshot snap;
  // ... populate from cached_market_state_ + position state
  trailing_sl_->tick(snap);
}
```

Вызывается в `run_periodic_tasks()` после verify_brackets loop.
