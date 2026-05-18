# Stagnant Position Detector

**Файлы:** `stagnant_position_detector.{hpp,cpp}`
**Введено:** run90 (2026-05-17)

## Назначение

Detect позиции которые "застыли" (мало движения, не идут ни к TP ни к SL) и в **loss-zone**.
Force exit освобождает capital для активных пар. Pair НЕ добавляется в blacklist —
может вернуться через scanner естественно.

## Принцип работы

```
on_position_opened(symbol, opened_at_ns)
  → init tracker

[on each pipeline tick]
tick(symbol, current_price, now_ns, entry_price, position_side)
  → append sample to deque, evict samples > window_sec
  → if hold > hard_max_hold_sec (30 min): return true (HARD EXIT)
  → if hold < min_position_age_sec (15 min): return false (грейс)
  → calc current PnL bps
  → if pnl > -loss_zone_threshold_bps (30): return false (profit zone — оставляем trail)
  → compute price range over window (max - min) в bps
  → if range < min_range_bps (12): return true (STAGNANT в loss-zone)
  → return false

on_position_closed(symbol)
  → erase tracker
```

## API

```cpp
struct StagnantPositionConfig {
  int stagnant_check_window_sec{180};     // окно для price range
  double min_range_bps{12.0};             // < threshold = stagnant
  int min_position_age_sec{900};          // run95: 600 → 900 (15 min grace)
  double loss_zone_threshold_bps{30.0};   // run95: 5 → 30 (даём шанс recovery)
  int hard_max_hold_sec{1800};            // 30 min hard exit unconditional
  bool enabled{true};
};

class StagnantPositionDetector {
  void on_position_opened(symbol, now_ns);
  void on_position_closed(symbol);
  void reset(symbol);
  bool tick(symbol, current_price, now_ns, entry_price, ps);
};
```

## Эволюция

| version | min_range_bps | min_position_age_sec | loss_zone | hard_max |
|---------|---|---|---|---|
| run90 initial | 8 | 180 | 5 (вообще не учитывалось) | - |
| run93 fix | 12 | 600 | 5 | - |
| **run95 fix** | 12 | **900** | **30** | **1800** |

## Run93 evidence: forced 3/3 losses до TP — слишком агрессивно

В run93: 3/3 закрытий = stagnant force exit за -8 bps до -27 bps в loss-zone.
Trail SL не успевал → small losses. Fix: detector только в loss-zone (НЕ profit-zone)
+ poднял threshold с 5 до 30 bps.

## Интеграция в trading_pipeline.cpp

```cpp
if (stagnant_detector_ && snap.current_price > 0.0 && !close_order_pending_) {
  bool stagnant = stagnant_detector_->tick(symbol_, snap.current_price, now_ns,
                                              snap.entry_price, current_position_side_);
  if (stagnant) {
    // 1. Cancel TP/SL plan ордера через bracket_manager.release()
    // 2. Emit emergency close intent через execution_engine
    //    с urgency=1.0, Aggressive (market), exit_reason=EmergencyExit
  }
}
```
