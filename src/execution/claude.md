# `src/execution` — Движок исполнения

## Назначение

Единственный шлюз отправки ордеров на биржу. Принимает `TradeIntent + RiskDecision + ExecutionAlphaResult + UncertaintySnapshot`, делегирует подсистемам (registry, planner, fill_processor, cancel_manager, recovery_manager, exec_metrics), вызывает `IOrderSubmitter`, возвращает `OrderId`.

> **Maker-first execution (scalping refactor 2026-05):**
> * `ExecutionPlanner::choose_style` теперь маппит `ExecutionStyle::PostOnly`/`Passive` в `PlannedExecutionStyle::PostOnlyLimit` (раньше шло в `PassiveLimit`/`Limit` без `force=post_only` — фактически терялась maker-гарантия).
> * `FillProcessor` различает maker/taker fees: для `OrderType::PostOnly`/`Limit`/`StopLimit` используется `kDefaultMakerFeePct = 0.0002` (2 bps), для `Market`/`StopMarket` — `kDefaultTakerFeePct = 0.0006` (6 bps). Раньше всё считалось как taker → искажённый PnL для maker-fills.
> * Defaults: `urgency_aggressive_threshold = 0.80` (раньше 0.50), `postonly_spread_threshold_bps = 12.0` (раньше 8.0). С `intent.urgency = 0.30` от strategy_engine EV-modeling в execution_alpha теперь по умолчанию выбирает PostOnly.

> **edge-31 — Exchange-attached TP/SL (2026-05-16):**
> * `ExecutionEngine::create_order_record` копирует `attached_tp_sl` из `intent.take_profit_price` / `intent.stop_loss_price` ТОЛЬКО для `TradeSide::Open`. Close/Reduce ордера не несут bracket.
> * `BitgetFuturesOrderSubmitter::build_place_order_json` сериализует `presetStopSurplusPrice` / `presetStopLossPrice` если `attached_tp_sl.has_any()`. Trigger type — MarkPrice (защита от wick stop hunts).
> * `submit_plan_order` / `cancel_plan_order` помечены `virtual` — используются `ProtectiveBracketManager` для standalone fallback и trailing cancel-and-replace.

## Границы ответственности

* Валидация input'ов (intent vs risk vs exec_alpha consistency).
* Дедупликация ordering intent'ов.
* Резервирование margin (`portfolio.reserve_cash`).
* Создание `OrderRecord` + регистрация FSM в `OrderRegistry`.
* Планирование исполнения (`ExecutionPlanner` — order type, timeout, fallback).
* Submit через `IOrderSubmitter`.
* Обработка fill/order events (через подсистемы).
* Cancel: per-order, per-symbol, all, emergency-flatten.
* Reconciliation: запуск, mismatch resolution.
* TWAP execution через `SmartTwapExecutor`.
* Метрики исполнения.

## Структура каталога

| Под-каталог | Назначение |
|-------------|------------|
| `orders/`     | `OrderRegistry`, `client_order_id` helpers |
| `planner/`    | `ExecutionPlanner` — план исполнения |
| `fills/`      | `FillProcessor` — применение fills к portfolio |
| `cancel/`     | `CancelManager` — отмены |
| `recovery/`   | `RecoveryManager` — восстановление state ордеров |
| `telemetry/`  | `ExecutionMetrics` — Prometheus-метрики |

## Публичные интерфейсы

* `class ExecutionEngine` (главный класс).
* `class IOrderSubmitter` — интерфейс, реализуется `BitgetFuturesOrderSubmitter`.
* `class SmartTwapExecutor` — для крупных ордеров, разбиение на slice'ы.
* `class OrderFSM` — состояния ордера (New → PendingNew → Open → PartiallyFilled → Filled / Cancelled / Rejected / Expired).
* `class ExecutionQualityMonitor` — slippage, fill rate analytics.
* `OrderRecord`, `OrderState`, `OrderTransition`, `FillEvent`, `OrderExecutionInfo`, `ExecutionPlan`, `ReconciliationResult`, `ExecutionStats`, `PartialFillPolicy`.
* `ExecutionConfig` — пороги, таймауты, min_notional, max age, fill policies.

## Внутренние компоненты

* `execution_engine.hpp/cpp` — главный orchestrator.
* `order_fsm.hpp/cpp` — конечный автомат.
* `order_types.hpp` — DTO.
* `order_submitter.hpp` — interface.
* `execution_types.hpp`, `execution_config.hpp`, `execution_utils.hpp`.
* `twap_executor.hpp/cpp` — TWAP slicing.
* `execution_quality_monitor.hpp/cpp` — quality metrics.
* `orders/`, `planner/`, `fills/`, `cancel/`, `recovery/`, `telemetry/` — подсистемы.

## Зависимости

* `strategy`, `risk`, `execution_alpha`, `uncertainty`, `portfolio`.
* `common`, `clock`, `logging`, `metrics`.

## Потоки данных

```
execute(intent, risk, exec_alpha, uncertainty):
  lock(execute_mutex_)                 ← H-4: prevents dedup TOCTOU
  validate_inputs(...)
  if registry_.is_duplicate(intent): return Err(IdempotencyDuplicate)
  plan = planner_.plan(intent, risk, exec_alpha, config_)
  order = create_order_record(intent, risk, exec_alpha, plan)
  if plan.requires_margin_reservation:
     try_reserve_margin(order)         ← portfolio_.reserve_cash
     if fail: return Err
  registry_.register_order(order)
  result = submitter_.submit_order(order)
  if !result.success:
     registry_.transition(o.id, Rejected)
     portfolio_.release_cash(o.id)
     exec_metrics_.record_reject(...)
     return Err
  registry_.transition(o.id, PendingNew or Open)
  return Ok(o.order_id)

on_fill_event(fill):
  fill_processor_.apply(fill, registry_, portfolio_)
  if FullyFilled: registry_.transition(Filled), portfolio_.open_position
  if PartiallyFilled: portfolio_.reduce_position (для close) или partial open

on_order_update(o, new_state, ...):
  registry_.transition(o, new_state)
  если Rejected: release_cash

cancel(o): submitter_.cancel_order, registry_.transition(Cancelled)
cancel_timed_out_orders(max_age): итерируем active, cancel
```

## Race conditions

* `execute_mutex_` сериализует execute().
* `OrderRegistry` — внутренний mutex.
* `submitter_` — REST вызовы (potentially blocking).
* `on_fill_event`/`on_order_update` могут вызываться из private WS thread → возможен race с execute (защищено через registry mutex).

## Ошибки проектирования

* **D-ex-1 (HIGH).** `try_reserve_margin` происходит ВНУТРИ `execute_mutex_`; при отказе resv не атомарен с `is_duplicate` — возможен ghost reservation на отклонённом дублирующемся intent. См. **Defect-D9 в корне**.
* **D-ex-2 (MEDIUM).** Сама execute() блокирующая через `submitter_.submit_order` (REST). Под `execute_mutex_` это блокирует все concurrent ордера. Mitigation: разделить регистрацию (synchronous) и submit (async).
* **D-ex-3 (MEDIUM).** `cancel_timed_out_orders` копирует `OrderRecord` для каждого cancel'а — при 100+ pending heavy.
* **D-ex-4 (MEDIUM).** `cleanup_terminal_orders` должен вызываться периодически — кто это делает? Возможно, watchdog в pipeline. Verify.
* **D-ex-5 (LOW).** `set_leverage(double)` в ExecutionEngine — но реальный leverage должен быть integer; double хранится для совместимости с конфигурацией.

## Контракты

См. § 9.2 в корневом `claude.md`. Дополнительно:

### `cancel_all_for_symbol(symbol) → vector<OrderId>`

* **Pre.** Никаких.
* **Post.** Возвращён список cancelled order IDs. Для каждого `cancel(o)` была попытка; неуспешные cancel'ы лишь логируются.

### `emergency_flatten_symbol(symbol) → VoidResult`

* **Pre.** Symbol валидный.
* **Post.** Все pending ордера cancelled, все позиции (Long+Short) закрыты market-ордерами.
* **Invariant.** После `emergency_flatten`: `portfolio.has_position(symbol) = false`.

### `run_reconciliation() → ReconciliationResult`

* **Pre.** `recovery_manager_` готов, `submitter_` доступен.
* **Post.** Возвращён результат с mismatches и auto-resolved.

## Производственные риски

* **R-ex-1.** Submit под mutex'ом → блокирует concurrent ордера. При сетевом jitter критично.
* **R-ex-2.** `emergency_flatten`: market ордер на закрытие при тонкой книге → большой slippage. Но это аварийный режим, riск приемлем.
* **R-ex-3.** Idempotency: при reconnect submitter после успешного fill — fill_event может быть пропущен. Mitigation: периодический `run_reconciliation`.

## Рекомендации

1. **R-ex-Big.** Async submit на dedicated thread; execute() возвращает `future<OrderId>` или регистрирует callback.
2. Atomic resv+register: изолировать резервацию в одну операцию (TODO для Defect-D9).
3. Метрики: `execution_submit_latency_ms`, `execution_fill_latency_ms`, `execution_cancel_latency_ms`, `execution_orders_active` (gauge), `execution_orders_terminal_total{state}`.
4. Тест: TSAN на 1000 concurrent execute из mock pipeline, проверка инвариантов registry.
5. Documenter контракт `IOrderSubmitter` (что должен делать adapter, какие ошибки маппить).
