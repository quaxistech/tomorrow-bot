# `src/order_book` — Локальный L2 стакан

## Назначение

Поддержание актуального снимка книги ордеров (bids/asks) на основе snapshot+delta потока от биржи. Контроль целостности (sequence, crossed-book), экспорт top-of-book и depth summary для downstream feature/risk модулей.

## Границы ответственности

* Применение `NormalizedOrderBook` snapshots и deltas.
* Контроль `sequence_continuity` — Bitget шлёт `update_id` инкрементально.
* Детекция «перекрещивания» (`bid >= ask`) — индикатор бага парсинга.
* Экспорт: `top_of_book`, `depth_summary(N)`, полные `bids()`/`asks()`.
* Подписка downstream `BookEventCallback` на `BookEventBatch`.

## Границы (что НЕ делает)

* Не считает features (это `FeatureEngine`).
* Не вычисляет toxicity/VPIN (это `AdvancedFeatureEngine`).
* Не агрегирует по тиковым интервалам (это `IndicatorEngine`).

## Входы / выходы

* Вход: `apply_snapshot(NormalizedOrderBook)`, `apply_delta(NormalizedOrderBook)`.
* Выход: `top_of_book`, `depth_summary`, `BookEventBatch` через callbacks.

## Публичные интерфейсы

* `class LocalOrderBook`:
  * Конструктор `LocalOrderBook(symbol, logger, metrics)`.
  * `apply_snapshot(...)`, `apply_delta(...) → bool` (false при desync).
  * `request_resync()` — флаг для downstream.
  * `check_staleness(now, threshold_ns) → bool`.
  * `quality() → BookQuality {Uninitialized, OK, Stale, OutOfSync, Crossed, Sparse}`.
  * `top_of_book() → optional<TopOfBook>`.
  * `depth_summary(levels = 10) → optional<DepthSummary>`.
  * `bids() / asks()` — копии `std::map`.
  * `subscribe(BookEventCallback)`.
* `BookEvent` — `LevelAdded`, `LevelRemoved`, `LevelUpdated`, `BookCleared`.
* `BookEventBatch` — vector + `Timestamp`.

## Внутренние компоненты

* `order_book.hpp/cpp` — `LocalOrderBook`.
* `order_book_types.hpp` — `BookQuality`, `TopOfBook`, `DepthSummary`, `BookSide`, `BookEvent`, `BookEventBatch`.

## Зависимости

* `normalizer/normalized_events.hpp`.
* `logging`, `metrics`.

## Потоки данных

```
NormalizedOrderBook (snapshot or delta) →
  apply_snapshot/delta:
    lock(mutex_)
    if snapshot:
      reset bids_/asks_ → fill from levels → set last_sequence_, quality=OK
    if delta:
      check sequence_ continuity → OK или quality=OutOfSync, request_resync, return false
      apply_book_levels_with_events(side, levels, BookSide, ts, batch)
    detect_crossed_book → quality=Crossed на бизаре
    update metrics: snapshots_counter / deltas_counter / desyncs / crossed
    emit_events(batch) → subscribers_[i](batch)
```

## Race conditions

* Все mutating и read operations под `mutex_`.
* `subscribe` не имеет mutex — добавление subscriber во время `emit_events` создаёт race (см. **Defect D7 в корне**).
* `subscribers_` итерируется без копии — если callback вызывает `subscribe`, UB.

## Ошибки проектирования

* **D-ob-1 (MEDIUM).** `std::map<Price,Quantity, std::greater<Price>>` для bids — heap-аллокации на каждый уровень при `apply_delta`. Для активного стакана (BTCUSDT, 100+ обновлений/sec) это hot-spot. Mitigation: flat sorted vector или `absl::btree_map`.
* **D-ob-2 (MEDIUM).** `subscribe()` без mutex_ — race с iterating subscribers (см. **D7**).
* **D-ob-3 (LOW).** `bids()`/`asks()` возвращают копии `std::map` — каждый вызов из feature engine клонирует всю книгу. Если делается каждый тик — большой overhead. Лучше предоставлять `read_bids(F&&)` callback.
* **D-ob-4 (LOW).** `BookQuality::Sparse` упомянут в типах, но условие активации (минимум уровней) не очевидно из API — должно быть в config.

## Контракты

### `apply_snapshot(NormalizedOrderBook snap)`

* **Pre.** `snap.is_snapshot = true`. `snap.bids` отсортирован по убыванию цены, `snap.asks` — по возрастанию (или нормализатор приводит).
* **Post.** `bids_/asks_` заполнены, `last_sequence_ = snap.sequence`, `quality_ = OK`, `last_updated_ = snap.timestamp`. Метрика `snapshots_counter` инкрементирована.
* **Invariant.** Inv-7 (sequence continuity) reset.

### `apply_delta(NormalizedOrderBook delta) → bool`

* **Pre.** Snapshot уже был применён (`last_sequence_ != 0`).
* **Post.**
  * Если `delta.sequence > last_sequence_`: применить, обновить sequence, emit events, return `true`.
  * Иначе: quality=OutOfSync, request_resync, return `false`.
* **Invariant.** Книга не «перекрещена» после применения (если detected — quality=Crossed).

### `top_of_book()`

* **Pre.** `quality ∈ {OK}` (для надёжного результата).
* **Post.** Возвращён `optional<TopOfBook>{best_bid, best_ask}`. `nullopt` если книга пуста или quality деградировал.

## Производственные риски

* **R-ob-1.** Sequence gap из-за пакета сетевого джиттера → `OutOfSync` → нужен resync. Если resync request не отрабатывает (нет API метода?), книга остаётся в плохом состоянии. Mitigation: автоматический unsubscribe+resubscribe в gateway.
* **R-ob-2.** Crossed book → algorithm violation → стратегии не должны торговать. Сейчас `quality_` экспортируется, но downstream должен явно проверять.

## Рекомендации

1. Перейти на cache-friendly структуру (flat sorted vector / `boost::flat_map`).
2. Защитить `subscribe()` mutex'ом + копировать subscribers list в `emit_events`.
3. `read_bids/read_asks(F)` API без копирования.
4. Метрика `book_apply_delta_latency_ns` — гистограмма.
5. Тест: smoke fuzzing на out-of-order deltas, проверка восстановления.
