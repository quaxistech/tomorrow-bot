# `src/execution/planner` — Планировщик исполнения

## Назначение

Преобразует `TradeIntent + RiskDecision + ExecutionAlphaResult` в `ExecutionPlan` (тип ордера, цена, таймаут, fallback policy, partial fill policy).

## Границы ответственности

* Mapping `ExecutionStyle` → `OrderType` (Passive→Limit, Aggressive→Market, PostOnly→PostOnly, ...).
* Расчёт таймаута жизни лимит-ордера до auto-cancel.
* Fallback стратегия (если limit не fill в течение T → market).
* Расчёт окончательной цены (с учётом slippage budget из RiskDecision).
* Partial fill policy (WaitForFull / CancelOnPartial / TolerateBest).

## Публичные интерфейсы

* `class ExecutionPlanner`:
  * Конструктор `(ExecutionConfig, ILogger)`.
  * `plan(intent, risk, exec_alpha) → ExecutionPlan`.
* `ExecutionPlan` — `{order_type, limit_price, timeout_ms, fallback_type, partial_fill_policy, slice_plan, …}`.

## Внутренние компоненты

* `execution_planner.hpp/cpp`.

## Зависимости

* `strategy`, `risk`, `execution_alpha`.
* `execution/execution_types.hpp`.

## Потоки данных

`ExecutionEngine::execute → planner_.plan(intent, risk, exec_alpha) → plan`.

## Race conditions

Stateless (предположительно).

## Ошибки проектирования

* **D-pln-1 (LOW).** Если `exec_alpha.style = NoExecution`, planner всё ещё вызывается — должно быть отсечено выше в `validate_inputs`. Verify.

## Контракты

### `plan(intent, risk, exec_alpha) → ExecutionPlan`

* **Pre.** `risk.allowed = true`. `exec_alpha.style != NoExecution`.
* **Post.**
  * `plan.order_type ∈ {Limit, Market, PostOnly, StopMarket, StopLimit}` соответственно.
  * `plan.timeout_ms > 0` для limit ордеров.
  * `plan.partial_fill_policy` определён.

## Производственные риски

* **R-pln-1.** Mis-mapping style → type приведёт к Bitget rejection.

## Рекомендации

1. Property-based тест: для каждого `ExecutionStyle` соответствует валидный `OrderType`.
2. Документировать таблицу маппинга style→type.
