# `src/execution/recovery` — Восстановление состояния ордеров

## Назначение

Reconciliation сторон ордеров (local registry vs exchange) с авто-разрешением расхождений: missing local (создать record), missing exchange (transition to Cancelled/Filled), state mismatch (force_transition).

## Границы ответственности

* `run_reconciliation() → ReconciliationResult`.
* Сравнение всех active local orders с биржей.
* Auto-resolution для типичных мисматчей.
* Эскалация к оператору для unresolvable cases.

## Публичные интерфейсы

* `class RecoveryManager`:
  * Конструктор `(IOrderSubmitter (для query), OrderRegistry&, ILogger, IMetricsRegistry)`.
  * `run_reconciliation() → ReconciliationResult`.
* `ReconciliationResult` — `{mismatches, auto_resolved, operator_escalated}`.

## Внутренние компоненты

* `recovery_manager.hpp/cpp`.

## Зависимости

* `execution/order_submitter.hpp`, `execution/orders/order_registry.hpp`.

## Потоки данных

`ExecutionEngine::run_reconciliation` → `recovery_manager_.run_reconciliation()` → итерация `registry_.active_orders()` → для каждого `query_order_status` → comparison → resolution.

## Race conditions

`run_reconciliation` сам по себе не должен исполняться concurrently (см. `ReconciliationEngine::op_mutex_`). Здесь — собственная логика.

## Ошибки проектирования

* **D-erc-1 (MEDIUM).** `force_transition` использует bypass FSM; risk масштабнее, чем normal cancel/fill — необходимо строгое логирование.
* **D-erc-2 (MEDIUM).** Дублирует функциональность с `reconciliation/ReconciliationEngine`. Не очевидно, какой запускается когда.

## Контракты

### `run_reconciliation() → ReconciliationResult`

* **Pre.** Submitter готов, registry не пустой.
* **Post.** Возвращён result с counts mismatches/auto_resolved/operator_escalated. Registry обновлён для auto-resolved.

## Производственные риски

* **R-erc-1.** Если query_order_status throws (network) — reconciliation incomplete; mismatch остаётся.

## Рекомендации

1. Объединить с `reconciliation/ReconciliationEngine` либо чётко разграничить ответственности.
2. Метрика `execution_recovery_mismatches_total{type}`.
3. Тест: симуляция всех типов mismatch (missing local/exchange, state divergence) и проверка auto-resolution.
