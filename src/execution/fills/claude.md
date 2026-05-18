# `src/execution/fills` — Обработчик fills

## Назначение

Применение `FillEvent` к `OrderRegistry` и `IPortfolioEngine`. Гарантирует идемпотентность (через `OrderRegistry::is_fill_applied`).

## Границы ответственности

* `apply(fill, registry, portfolio)` — главный entry.
* Различение partial vs full fill.
* Открытие/уменьшение позиции по `tradeSide` (Open→`portfolio.open_position`, Close→`portfolio.reduce_position`).
* Запись fee, slippage, real fill price.
* Update FSM state (Filled / PartiallyFilled).
* Release reserved cash для terminal fill.

## Публичные интерфейсы

* `class FillProcessor`:
  * Конструктор `(IPortfolioEngine, ILogger, IMetricsRegistry)`.
  * `apply(FillEvent, OrderRegistry&)`.

## Внутренние компоненты

* `fill_processor.hpp/cpp`.

## Зависимости

* `execution/order_types.hpp`, `execution/orders/order_registry.hpp`.
* `portfolio/portfolio_engine.hpp`.

## Потоки данных

```
ExecutionEngine::on_fill_event(fill):
  fill_processor_.apply(fill, registry_, portfolio_)
    if registry_.is_fill_applied(fill.order_id): return // идемпотентно
    order = registry_.get_order(fill.order_id)
    update order.filled_qty / order.avg_fill_price
    if fully filled:
      registry_.transition(Filled)
      registry_.mark_fill_applied
      portfolio_.release_cash + open_position / reduce_position
    else:
      registry_.transition(PartiallyFilled)
      portfolio_.update partial reservation
    portfolio_.record_fee(symbol, fee, order_id)
```

## Race conditions

Под `OrderRegistry::mutex_` для apply transitions.

## Ошибки проектирования

* **D-fp-1 (MEDIUM).** Atomicity между transition и portfolio update: если portfolio operation throws — registry уже в Filled, но позиция не открыта. Mitigation: try/catch + rollback FSM.
* **D-fp-2 (LOW).** `record_fee` использует `order_id` как ключ — fee хранится по ордеру. Если fill multiple events → требуется аккумуляция.

## Контракты

### `apply(FillEvent fill, OrderRegistry& registry)`

* **Pre.** `fill.order_id` существует в registry. `fill.qty > 0 ∧ fill.price > 0`.
* **Post.**
  * Если `is_fill_applied(fill.order_id) = true`: no-op (idempotent).
  * Иначе: registry FSM обновлен, portfolio обновлён, mark_fill_applied вызван (для terminal fills).
* **Invariant.** Inv-3 (идемпотентность fills) — нарушение → kill switch.

## run92 critical fix: WS fill price=0 handling

Bitget WS fill channel периодически шлёт `fillPx=0` для market orders. **Раньше (run91)** мы применяли fallback price из `order.execution_info.expected_fill_price`. Это создало **double-apply bug** (AIGENSYNUSDT):
1. WS event с price=0 → fallback applied → portfolio size=140
2. REST confirmation через 3ms с реальной ценой → REST process apply (другой trade_id, dedup не сработал) → portfolio size=280
3. На бирже реально 140 → phantom cleanup через 2 мин обнулил local → exchange-attached SL trigger потом = loss с unreported PnL.

**Решение (run92):** WS event с price=0 теперь **просто skip** (`return false`). REST polling (`process_market_fill`) подтянет fill с реальной ценой однократно. Trade-off: market order может ненадолго (1-3s) висеть в PendingAck до REST confirmation — приемлемо vs sync break.

## Производственные риски

* **R-fp-1.** Inconsistency между registry и portfolio при partial failure.
* **R-fp-2.** Дублированный fill event от Bitget (commonly happens) → must be filtered by `is_fill_applied`.

## Рекомендации

1. Atomic apply: одна транзакция registry + portfolio (в рамках одного mutex захвата).
2. Тест: дублированные fill events не приводят к double-counting.
3. Метрика `fill_apply_latency_ns`, `fill_duplicates_total`.
