# `src/recovery` — Восстановление при старте

## Назначение

Полное восстановление состояния системы при старте: позиции и баланс с биржи, journal replay из persistence, синхронизация локального портфеля. Гарантирует, что бот после рестарта корректно отражает реальное состояние.

## Границы ответственности

* `recover_on_startup()` — полный recovery (вызывается ДО запуска pipeline'ов).
* `recover_positions()` — после reconnect.
* `recover_from_journal()` — replay WAL.
* `recover_full_state()` — extended (positions + pending orders + protective TP/SL + pair-state inference).

## Публичные интерфейсы

* `class RecoveryService`:
  * Конструктор `(RecoveryConfig, IExchangeQueryService, IPortfolioEngine, PersistenceLayer, ILogger, IClock, IMetricsRegistry)`.
  * `recover_on_startup() → RecoveryResult`.
  * `recover_positions() → RecoveryResult`.
  * `recover_from_journal() → RecoveryResult`.
  * `recover_full_state() → ExtendedRecoveryResult`.
  * `last_result()`, `status()`, `last_extended_result()`.
* DTO: `RecoveryResult{status, recovered_positions, errors}`, `RecoveryStatus enum`, `ExtendedRecoveryResult` (расширенный).

## Внутренние компоненты

* `recovery_service.hpp/cpp`.
* `recovery_types.hpp`.

## Зависимости

* `reconciliation/reconciliation_engine.hpp` (для query service).
* `portfolio/portfolio_engine.hpp`.
* `persistence/persistence_layer.hpp`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
recover_on_startup:
  exchange_balances = exchange_query_->get_account_balances()
  exchange_positions = exchange_query_->get_open_positions()
  for each ex_pos:
    portfolio_->sync_position_from_exchange(symbol, ps, qty, entry_price, current_price, unrealized, opened_at)
  cash = extract_usdt_balance(exchange_balances)
  portfolio_->set_capital(cash)
  return RecoveryResult{Success, recovered_positions=N}

recover_full_state (extended):
  recover_on_startup
  + pending orders sync (через RecoveryManager)
  + protective TP/SL detection (через get_trigger_orders)
  + pair-state inference (если есть locked Long+Short, помечает как hedge)

recover_from_journal:
  snapshot_ts = restore_from_snapshot()  // последний снимок
  replay_journal_after_snapshot(snapshot_ts)
```

## Race conditions

* `mutex_` для last_result.
* Recovery вызывается до запуска pipeline → один поток.

## Ошибки проектирования

* **D-rcv-1 (HIGH).** `recover_full_state` существует, но в pipeline в `main.cpp` напрямую вызывается `recover_on_startup` (TBD verify exact path) — extended recovery может быть не на main path → trigger orders не recovered. См. **R-9 в корне**.
* **D-rcv-2 (MEDIUM).** При журнале большой длины replay может занять много времени. Нет промежуточного reporting.
* **D-rcv-3 (MEDIUM).** Snapshot ↔ journal merge: если snapshot за 30 сек до restart, а journal содержит события после — реплей корректен. Но если snapshot позже самого старого journal event — mismatch.

## Контракты

### `recover_on_startup() → RecoveryResult`

* **Pre.** Exchange query доступен, portfolio пустой (свежесозданный).
* **Post.**
  * Успех: `result.status = Success`, portfolio отражает биржу.
  * Частично: `result.status = Partial`, `errors` содержит проблемы.
  * Полная неудача: `result.status = Failed`.
* **Invariant.** После Success: portfolio.capital и positions == exchange truth.

### `recover_full_state() → ExtendedRecoveryResult`

* **Pre.** Аналогично + persistence доступен.
* **Post.** Все active orders / TP/SL trigger-ордера загружены в `OrderRegistry`. Pair-state корректно классифицирован.

## Производственные риски

* **R-rcv-1.** **Critical.** Если trigger TP/SL ордер на бирже не загружен в registry, при рестарте бот не знает о protective ордере — может закрыть позицию вручную, не учитывая trigger. Двойной exit.
* **R-rcv-2.** Journal replay assumes deterministic state machine; любые недетерминированные операции (например, `clock_->now()`) приведут к divergence.

## Рекомендации

1. **R-rcv-Big.** Гарантировать, что `recover_full_state` вызывается на main path в `app/main.cpp`. Добавить assertion.
2. Прогресс в логе: «Recovered 23/100 events from journal».
3. Тест: симуляция рестарта в pipeline после различных событий, проверка консистентности state.
4. Метрика `recovery_duration_seconds`, `recovery_positions_recovered`, `recovery_journal_events_replayed`.
