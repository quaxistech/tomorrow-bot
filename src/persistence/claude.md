# `src/persistence` — Слой персистентности

## Назначение

Долгосрочное хранение: журнал событий (WAL) для replay и снимки состояния портфеля. Адаптеры: in-memory (для тестов) и PostgreSQL (production).

## Границы ответственности

* `EventJournal` — append-only WAL событий портфеля/ордеров/решений.
* `SnapshotStore` — периодические снимки состояния (portfolio + active orders + ML state).
* `IStorageAdapter` — абстракция над хранилищем.
* `MemoryStorageAdapter` — in-memory.
* `PostgresStorageAdapter` — libpqxx, schema-managed.
* `WalWriter` — низкоуровневый writer.
* `PersistenceLayer` — facade.

## Публичные интерфейсы

* `class PersistenceLayer`:
  * Конструктор `(IStorageAdapter, PersistenceConfig)`.
  * `journal() → EventJournal&`.
  * `snapshots() → SnapshotStore&`.
  * `flush() → VoidResult`.
  * `is_enabled()`.
* `class EventJournal` — `append(JournalEvent)`, `range(from_ts, to_ts) → vector<JournalEvent>`.
* `class SnapshotStore` — `save(Snapshot)`, `load_latest() → optional<Snapshot>`, `load_at_or_before(ts)`.
* `class IStorageAdapter` — abstract.
* `class WalWriter` — append-only file (для backup-уровня).

## Внутренние компоненты

* `persistence_layer.hpp/cpp` — facade.
* `event_journal.hpp/cpp`.
* `snapshot_store.hpp/cpp`.
* `storage_adapter.hpp` — interface.
* `memory_storage_adapter.hpp/cpp`.
* `postgres_storage_adapter.hpp/cpp`.
* `wal_writer.hpp/cpp`.
* `persistence_types.hpp` — DTO.

## Зависимости

* libpqxx (PostgreSQL).
* `common`, `clock`, `logging`.

## Потоки данных

```
TradingPipeline::persist_portfolio_snapshot (каждые 30s):
  snapshot = build_snapshot(portfolio, ml, registry)
  persistence_->snapshots().save(snapshot)

TradingPipeline::persist_position_event (на каждое open/close):
  event = build_event(...)
  persistence_->journal().append(event)

flush() periodic — sync to disk/db
```

## Race conditions

* EventJournal и SnapshotStore — каждый имеет свой mutex (thread-safe per facade comment).
* Adapters thread-safe (Postgres connection pool managed внутри).

## Ошибки проектирования

* **D-prs-1 (MEDIUM).** `flush` синхронный; при slow PostgreSQL блокирует hot-path. Mitigation: dedicated persistence thread + bounded queue.
* **D-prs-2 (MEDIUM).** Schema migration: нет встроенного механизма версионирования таблиц. Изменения требуют ручного DDL.
* **D-prs-3 (LOW).** `MemoryStorageAdapter` — для тестов; нет boundary check, может OOM при длительной работе.
* **D-prs-4 (LOW).** Нет recovery API в `EventJournal` (range) при неупорядоченных timestamps (clock skew).

## Контракты

### `EventJournal::append(event)`

* **Pre.** `event` имеет валидный timestamp.
* **Post.** Событие сохранено. После `flush` — durable.
* **Invariant.** Append-only: events не модифицируются после append.

### `SnapshotStore::save(snapshot)`

* **Pre.** `snapshot` валиден.
* **Post.** Снимок сохранён; `load_latest` вернёт его.

## Производственные риски

* **R-prs-1.** PostgreSQL down → TradingPipeline блокируется на flush → упустит hot-path tick. Mitigation: graceful degradation (fall back to in-memory + alert).
* **R-prs-2.** Disk full на WAL writer → write failure → потеря событий.

## Рекомендации

1. Async persistence: producer-consumer queue с TradingPipeline.
2. Schema versioning + migration framework.
3. Boundary check для `MemoryStorageAdapter`.
4. Тест: replay из journal восстанавливает state точно.
5. Метрика `persistence_lag_ms`, `persistence_queue_depth`.
