# `src/risk/state` — Stateful риск-компоненты

## Назначение

Централизованное состояние, разделяемое между `IRiskCheck` реализациями: locks, loss streak, daily PnL, drawdown.

## Границы ответственности

* `LockRegistry` — управление locks (Symbol/Strategy/Day/Account/EmergencyHalt/Cooldown), expire, query.
* `LossStreakTracker` — total/symbol/strategy consecutive losses, daily stopouts.
* `PnlTracker` — per-symbol/per-strategy daily PnL.
* `DrawdownTracker` — peak equity, account/intraday drawdown %.

## Публичные интерфейсы

* `class LockRegistry`:
  * `add_lock/remove_lock/clear_expired`.
  * `is_locked/has_*` queries.
  * `compute_global_state() → RiskStateLevel`.
* `class LossStreakTracker`:
  * `record_trade_result(symbol, strategy, is_loss, now)`.
  * `total_consecutive_losses()`, `symbol/strategy_consecutive_losses(...)`.
  * `daily_stopouts()`, `last_loss_time()`.
* `class PnlTracker`:
  * `record_trade_pnl(symbol, strategy, pnl)`.
  * `symbol/strategy_daily_pnl(...)`.
* `class DrawdownTracker`:
  * `update_equity(equity, now)`.
  * `peak_equity()`, `account_drawdown_pct()`, `intraday_drawdown_pct()`.

## Внутренние компоненты

* `risk_state.hpp/cpp` — все 4 класса в одном файле.

## Зависимости

* `risk/risk_types.hpp`, `common/types.hpp`.

## Потоки данных

`ProductionRiskEngine::evaluate` — read-only access (`is_locked`, `total_consecutive_losses`, `account_drawdown_pct`, …).
`record_trade_result/close/order_sent` — записывает new state.
`reset_daily` — сбрасывает дневные счётчики.

## Race conditions

Все компоненты работают под общим `RiskEngine::mutex_`. Внутренних mutex'ов нет.

## Ошибки проектирования

* **D-rsks-1 (LOW).** Нет персистентности: при перезапуске loss streak / daily PnL пропадают. Mitigation: dump в snapshot и восстановить через recovery.
* **D-rsks-2 (LOW).** `DrawdownTracker::reset_intraday` — отдельный метод от `reset_daily`; контракт когда вызывать какой не очевиден.

## Контракты

### `LockRegistry::add_lock(type, target, reason, now, duration_ns = 0)`

* **Pre.** `type` валидный. `target` пустой только для Day/Account/EmergencyHalt.
* **Post.** В `locks_` добавлена запись. `compute_global_state` отражает.

### `LossStreakTracker::record_trade_result(symbol, strategy, is_loss, now)`

* **Pre.** `symbol`, `strategy` непустые.
* **Post.** Если `is_loss = true`: total/symbol/strategy counters инкрементируются. Иначе — все обнуляются.
* **Invariant.** `daily_stopouts_` инкрементируется при достижении лимита (требует верификации в реализации).

## Производственные риски

* **R-rsks-1.** Перезапуск посреди дня → потеря state → пропуск day-lock. Critical.

## Рекомендации

1. Добавить snapshot/restore для всех 4 компонентов.
2. Вызов `reset_daily` явно по UTC-границе (через scheduled task в Supervisor).
3. Тест: simulate trading day, проверить инкременты и сброс.
