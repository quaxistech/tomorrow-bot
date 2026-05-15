# `src/execution/cancel` — Cancel Manager

## Назначение

Унифицированная отмена ордеров: per-order, per-symbol, all, emergency-flatten. Управление таймаутами и fallback'ом (если cancel неуспешен — escalate).

## Границы ответственности

* `cancel(order_id) → VoidResult`.
* `cancel_all_for_symbol(symbol) → vector<OrderId>`.
* `cancel_all() → vector<OrderId>`.
* `emergency_flatten_symbol(symbol) → VoidResult` (cancel + close открытых позиций).
* Cancel-timeout escalation (если submitter не отвечает).

## Публичные интерфейсы

* `class CancelManager`:
  * Конструктор `(IOrderSubmitter, OrderRegistry&, ILogger, IMetricsRegistry)`.
  * `cancel/cancel_all/emergency_flatten/...`.

## Внутренние компоненты

* `cancel_manager.hpp/cpp`.

## Зависимости

* `execution/order_submitter.hpp`, `execution/orders/order_registry.hpp`.
* `logging`, `metrics`.

## Потоки данных

```
cancel(o):
  rec = registry_.get_order(o)
  if !rec.has_value(): return Err
  if rec->state в terminal: return (no-op)
  ok = submitter_.cancel_order(o, rec->symbol)
  if ok: registry_.transition(o, Cancelled, "cancel-by-id")
  else: registry_.transition(o, ?, "cancel-failed")
  return result

emergency_flatten_symbol(s):
  cancel_all_for_symbol(s)
  for each open position (Long, Short):
    submit market close order (reduce_only = true)
```

## Race conditions

Submitter — blocking REST. Под mutex'ом registry — короткие операции.

## Ошибки проектирования

* **D-cm-1 (MEDIUM).** `emergency_flatten` отправляет market-close с `reduce_only=true` через `BitgetFuturesOrderSubmitter`; при тонкой книге slippage может быть большим. Для emergency приемлемо.
* **D-cm-2 (LOW).** Cancel результат `bool` не различает «уже отменён» vs «сетевой fail» vs «неизвестный order». Lossy semantic.

## Контракты

### `cancel(o) → VoidResult`

* **Pre.** `o` корректный.
* **Post.**
  * Успех: registry → Cancelled.
  * Неудача: state не меняется (или Rejected при exchange error).

### `emergency_flatten_symbol(symbol) → VoidResult`

* **Pre.** Symbol зарегистрирован.
* **Post.** Все pending → Cancelled. Все open positions → закрыты (или попытка close инициирована).
* **Invariant.** После успешного выполнения: `portfolio.has_position(symbol) = false`.

## Производственные риски

* **R-cm-1.** Cancel при partial fill: может оставить «висящий» partial fill, который потом продолжит исполняться.
* **R-cm-2.** Bitget rate-limit на cancel-many — глобальный rate budget.

## Рекомендации

1. Возвращать `enum class CancelResult { Cancelled, AlreadyTerminal, NetworkError, ExchangeError, NotFound }`.
2. Bulk cancel through Bitget batch endpoint (если доступен) — снижает rate-limit pressure.
3. Тест: race emergency_flatten + concurrent fill event.
