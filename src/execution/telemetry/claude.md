# `src/execution/telemetry` — Метрики исполнения

## Назначение

Prometheus-совместимые метрики hot-path исполнения: submit/fill/cancel latency, reject rate, slippage, fill rate per style, и т.д.

## Границы ответственности

* `record_submit(latency, success, style)`.
* `record_fill(latency, slippage, qty)`.
* `record_cancel(latency, success)`.
* `record_reject(reason)`.
* `execution_stats() → ExecutionStats` (snapshot).

## Публичные интерфейсы

* `class ExecutionMetrics`:
  * Конструктор `(IMetricsRegistry)`.
  * `record_*` методы.
  * `stats() → ExecutionStats`.
* `ExecutionStats` — DTO снимка.

## Внутренние компоненты

* `execution_metrics.hpp/cpp`.

## Зависимости

* `metrics/IMetricsRegistry`.
* `execution/execution_types.hpp`.

## Потоки данных

`ExecutionEngine` и подсистемы вызывают `record_*`; метрики экспортируются через `IMetricsRegistry::export_prometheus`.

## Race conditions

Атомарные counter/gauge/histogram; thread-safe.

## Ошибки проектирования

* **D-emt-1 (LOW).** Cardinality risk при добавлении тегов с высокой кардинальностью (например, per-strategy + per-symbol). См. `metrics/claude.md`.

## Контракты

Никаких особых; record-методы pure side-effect на metrics registry.

## Рекомендации

1. Стандартизировать имена метрик: `tb_execution_submit_latency_seconds`, `tb_execution_orders_total{style,result}`, `tb_execution_slippage_bps`.
2. Дашборд: распределение reject reasons по топ-5.
