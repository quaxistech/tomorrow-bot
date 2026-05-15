# `src/reconciliation` — Сверка с биржей

## Назначение

Сравнение локального состояния (orders, positions, balances) с реальным состоянием на бирже. Детекция расхождений и попытка авто-разрешения. Защита от drift, который ведёт к финансовым потерям.

## Границы ответственности

* `reconcile_on_startup` — полная сверка при запуске.
* `reconcile_active_orders` — периодически (60 с), только активные ордера.
* `reconcile_positions_and_balance` — периодически (5 мин), позиции + баланс USDT.
* `reconcile_single_order` — single-order check.
* Auto-resolution для типичных mismatches.

## Публичные интерфейсы

* `class ReconciliationEngine`:
  * Конструктор `(ReconciliationConfig, IExchangeQueryService, ILogger, IClock, IMetricsRegistry)`.
  * `reconcile_on_startup(local_orders, local_positions, local_cash) → ReconciliationResult`.
  * `reconcile_active_orders(local_active) → ReconciliationResult`.
  * `reconcile_positions_and_balance(local_positions, local_cash) → ReconciliationResult`.
  * `reconcile_single_order(local) → optional<MismatchRecord>`.
  * `last_result() → ReconciliationResult`.
* `class IExchangeQueryService`:
  * `get_open_orders(symbol="") → Result<vector<ExchangeOrderInfo>>`.
  * `get_account_balances() → Result<vector<ExchangePositionInfo>>`.
  * `get_open_positions(symbol="") → Result<vector<ExchangeOpenPositionInfo>>`.
  * `get_order_status(order_id, symbol) → Result<ExchangeOrderInfo>`.
  * `get_trigger_orders(symbol="") → Result<vector<ExchangeOrderInfo>>` (default empty).
* DTO: `ReconciliationConfig`, `ReconciliationResult`, `MismatchRecord`, `ExchangeOrderInfo`, `ExchangePositionInfo`, `ExchangeOpenPositionInfo`.

## Внутренние компоненты

* `reconciliation_engine.hpp/cpp`.
* `reconciliation_types.hpp`.

## Зависимости

* `execution/order_types.hpp` (OrderRecord).
* `portfolio/portfolio_types.hpp` (Position).
* `clock`, `logging`, `metrics`.

## Потоки данных

```
reconcile_on_startup(orders, positions, cash):
  lock(op_mutex_)               // BUG-S4-03: serialize concurrent runs
  ex_orders = exchange_query_->get_open_orders()
  ex_positions = exchange_query_->get_open_positions()
  ex_balances = exchange_query_->get_account_balances()
  mismatches = reconcile_orders(orders, ex_orders)
            + reconcile_positions(positions, ex_positions)
            + reconcile_balance(cash, ex_balances)
  auto_resolve_mismatches(result)
  finalize_result(result, start_ts)
  return result
```

## Race conditions

* `mutex_` для `last_result_`.
* `op_mutex_` — сериализация concurrent reconciliation runs.

## Ошибки проектирования

* **D-rec-1 (MEDIUM).** `IExchangeQueryService::get_trigger_orders` имеет default `return empty` — означает trigger ордера (TP/SL plan) НЕ сверяются с биржей! См. R-rec-1.
* **D-rec-2 (LOW).** `auto_resolve_mismatches` modifies result in-place; но не вызывает actions на portfolio — вызывающая сторона должна обработать результат.
* **D-rec-3 (LOW).** `reconcile_single_order` возвращает `optional<MismatchRecord>` — не проверяет всё, только single. Limited use.

## Контракты

### `reconcile_on_startup(local_orders, local_positions, local_cash)`

* **Pre.** Все local-параметры валидные. Exchange query доступен.
* **Post.** Возвращён `ReconciliationResult{mismatches, auto_resolved, operator_escalated, duration_ms}`.
* **Invariant.** Inv-12 (reconciliation freshness): по успешному завершению `last_reconciliation_ns` обновлён.

### `reconcile_active_orders(local_active)`

* **Pre.** `local_active` — список ордеров в active state.
* **Post.** Mismatches per order: missing_local / missing_exchange / state_diverged / qty_diverged.

## Производственные риски

* **R-rec-1.** Trigger orders (TP/SL) не сверяются → биржа может execute «зомби» trigger из прошлой сессии при reconnect. См. **R-9 в корне**.
* **R-rec-2.** При длительной reconciliation (медленный REST) — окно расхождений только увеличивается.
* **R-rec-3.** Auto-resolve может неверно интерпретировать race condition (новый ордер в момент сверки).

## Рекомендации

1. **R-rec-Big.** Реализовать `BitgetFuturesQueryAdapter::get_trigger_orders` и интегрировать в startup-reconciliation.
2. Async reconciliation в pipeline (см. R-2 в корне).
3. Метрика `reconciliation_mismatches_total{type}`, `reconciliation_duration_ms`.
4. Тест: симуляция всех типов mismatch, проверка auto-resolution не corrupt'нет state.
