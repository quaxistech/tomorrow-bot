# `src/metrics` — Prometheus-совместимые метрики

## Назначение

Реестр метрик (counter / gauge / histogram) с тегами и Prometheus exposition. Используется всеми компонентами для observability.

## Границы ответственности

* Регистрация и кэширование метрик по `(name, tags)`.
* Counter (monotonic), Gauge (set), Histogram (buckets, percentiles).
* Экспорт в text/plain `version=0.0.4` (Prometheus).
* HTTP endpoint поднимается в `app/http_server` с GET-запросом → `metrics->export_prometheus()`.

## Границы (что НЕ делает)

* Нет push-режима (только pull).
* Нет интеграции с OpenTelemetry/StatsD.
* Нет per-metric expire (метрики держатся до конца процесса).

## Публичные интерфейсы

* `class IMetricsRegistry`:
  * `std::shared_ptr<ICounter>   counter(name, tags = {}) `
  * `std::shared_ptr<IGauge>     gauge(name, tags = {}) `
  * `std::shared_ptr<IHistogram> histogram(name, tags = {}, buckets = ...)`
  * `std::string export_prometheus() const`
* `ICounter::increment(double = 1.0)`, `ICounter::value()`.
* `IGauge::set(double)`, `IGauge::inc/dec()`, `IGauge::value()`.
* `IHistogram::observe(double)`, percentiles.
* `metric_tags.hpp` — `std::vector<std::pair<std::string,std::string>>` тип.

## Внутренние компоненты

* `metrics_registry.hpp/cpp` — реестр + lookup по `(name + sorted_tags)` ключу.
* `counter.hpp` — атомарный счётчик.
* `gauge.hpp` — атомарный gauge.
* `histogram.hpp` — buckets + atomic counters; квантили вычисляются на экспорте.
* `metric_tags.hpp` — алиасы.

## Зависимости

* `<atomic>`, `<unordered_map>`, `<mutex>`.
* Используется всеми high-level модулями.

## Потоки данных

* Создание метрики: lookup → если нет, создать под mutex; вернуть `shared_ptr`.
* Запись: lock-free atomic ops.
* Экспорт: lock на reader-side, snapshot.

## Race conditions

* Регистрация под `mutex_`. Запись через `atomic`. Экспорт — короткий lock на iteration.
* Возможна ситуация: один поток создаёт метрику с тегами `{a=1}`, другой одновременно с `{a=1, b=2}` — это разные ключи, OK. Однако `export_prometheus` итерирует map, во время итерации новые метрики могут быть добавлены — поведение реализации требует верификации (вероятно, snapshot before iterate, или COW).

## Ошибки проектирования

* **D-met-1 (LOW).** Нет cardinality guard. Бесконтрольная регистрация метрик с уникальными тегами (например, `order_id`) приведёт к OOM. Mitigation: статический whitelist тегов или counter cardinality.
* **D-met-2 (LOW).** Histogram bucket'ы не настраиваются per-metric из YAML, только в коде. Не критично для текущего набора, но ограничивает observability.
* **D-met-3 (INFO).** Нет gc/expire устаревших метрик с динамическими тегами (например, метрики per-symbol при rotate).

## Контракты

### `IMetricsRegistry::counter(name, tags)`

* **Pre.** `name != ""`. `tags` не содержит дубликатов ключей.
* **Post.** Возвращён `shared_ptr<ICounter>`. Повторный вызов с той же `(name, tags)` возвращает тот же объект (idempotent).
* **Thread-safe.** Да.

### `IMetricsRegistry::export_prometheus()`

* **Pre.** Никаких.
* **Post.** Возвращена строка в формате Prometheus exposition v0.0.4.
* **Invariant.** Snapshot consistency: значения counter/gauge не меняются между чтением; для histogram — снимок quantile.

## Производственные риски

* **R-met-1.** Если HTTP endpoint держит lock на registry дольше, чем 100мс — может задерживать hot-path при высокой записи метрик.
* **R-met-2.** Cardinality blow-up: per-symbol метрики × 5+ дашбордов × `top_n` пар. На больших аккаунтах рост памяти заметен.

## Рекомендации

1. Ввести cardinality guard (max metrics, max unique tag-combos per name).
2. Бенчмарк: latency `counter->increment()` под нагрузкой 100K ops/s.
3. Сериализовать экспорт через `std::shared_mutex` (multiple readers, exclusive writer).
4. Тест: corner-case экспорта когда метрики удаляются/создаются во время iteration.
