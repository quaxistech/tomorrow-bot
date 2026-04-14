# Persistence Module Audit (Temporary)

Date: 2026-04-09

Scope of analysis:
- `src/persistence/persistence_types.hpp`
- `src/persistence/storage_adapter.hpp`
- `src/persistence/event_journal.hpp/.cpp`
- `src/persistence/snapshot_store.hpp/.cpp`
- `src/persistence/persistence_layer.hpp/.cpp`
- `src/persistence/memory_storage_adapter.hpp/.cpp`
- `src/persistence/postgres_storage_adapter.hpp/.cpp`
- `src/persistence/wal_writer.hpp/.cpp`
- `tests/unit/persistence/test_persistence.cpp`
- `tests/unit/persistence/test_wal_writer.cpp`
- integration points in `src/pipeline/trading_pipeline.cpp`
- dependent recovery path in `src/recovery/recovery_service.cpp`

## 1. Что делает модуль persistence

Модуль `persistence` в текущем проекте является инфраструктурным слоем хранения и восстановления состояния.

Его зона ответственности:

1. append-only журнал событий (`EventJournal`)
2. хранение последних снимков состояния (`SnapshotStore`)
3. единый фасад поверх этих двух механизмов (`PersistenceLayer`)
4. адаптеры конкретного хранения: память и PostgreSQL
5. отдельный `WalWriter` для write-ahead logging критических действий

Важно: модуль **не является** ни движком восстановления, ни бизнес-логикой портфеля, ни трейдинговым компонентом. Он даёт примитивы хранения; смысл событий и способ replay задаются внешними модулями, прежде всего `recovery`.

## 2. Архитектура модуля

Архитектура слоя трёхуровневая:

1. **Типы и контракты**
   - `JournalEntry`, `SnapshotEntry`, `PersistenceConfig`
   - `IStorageAdapter`

2. **Доменные оболочки над адаптером**
   - `EventJournal`
   - `SnapshotStore`
   - `PersistenceLayer`

3. **Конкретные backend’ы**
   - `MemoryStorageAdapter`
   - `PostgresStorageAdapter`
   - `WalWriter` как отдельный надстроечный механизм поверх `EventJournal`

По сути модуль строится вокруг идеи:

- бизнес-код не должен знать, где именно лежат журнал и снимки
- конкретный backend выбирается адаптером
- восстановление читает журнал и снимки через единый фасад

## 3. Состав файлов и роль каждого

### `persistence_types.hpp`

Определяет все базовые типы слоя:

- `JournalEntryType`
- `JournalEntry`
- `SnapshotType`
- `SnapshotEntry`
- `PersistenceConfig`

Это модель данных persistence-слоя.

### `storage_adapter.hpp`

Определяет интерфейс `IStorageAdapter` с пятью операциями:

- `append_journal(...)`
- `query_journal(...)`
- `store_snapshot(...)`
- `load_latest_snapshot(...)`
- `flush()`

Это главный seam между высокоуровневой логикой и конкретным хранилищем.

### `event_journal.hpp/.cpp`

`EventJournal` является thread-safe обёрткой над `IStorageAdapter` для append-only журнала.

Он:

- генерирует `sequence_id`
- ставит timestamp
- сериализует доступ через `mutex`
- делегирует физическую запись адаптеру

### `snapshot_store.hpp/.cpp`

`SnapshotStore` делает то же самое для снимков состояния:

- генерирует `snapshot_id`
- ставит `created_at`
- сохраняет payload JSON
- умеет загрузить последний снимок нужного типа

### `persistence_layer.hpp/.cpp`

Это фасад, который объединяет `EventJournal` и `SnapshotStore` за единым интерфейсом.

### `memory_storage_adapter.hpp/.cpp`

In-memory backend для тестов и fallback-сценариев.

### `postgres_storage_adapter.hpp/.cpp`

Продакшен-адаптер на PostgreSQL через `libpqxx`.

### `wal_writer.hpp/.cpp`

Дополнительный write-ahead logging слой для критических действий: ордера, позиции, checkpoint’ы.

## 4. Базовые типы данных

### 4.1 `JournalEntryType`

Модуль поддерживает следующие категории записей журнала:

- `MarketEvent`
- `DecisionTrace`
- `RiskDecision`
- `OrderEvent`
- `PortfolioChange`
- `StrategySignal`
- `SystemEvent`
- `TelemetrySnapshot`
- `GovernanceEvent`
- `DiagnosticEvent`

Это именно **категории хранения**, а не жёсткая схема payload’а.

### 4.2 `JournalEntry`

Поля записи журнала:

- `sequence_id` — монотонный идентификатор
- `type` — тип записи
- `timestamp` — время события
- `correlation_id` — связь с цепочкой обработки
- `strategy_id` — стратегия, если применимо
- `config_hash` — хэш конфига
- `payload_json` — непрозрачный JSON payload

Ключевая особенность: `payload_json` никак не валидируется на уровне persistence. Для модуля это просто строка.

### 4.3 `SnapshotEntry`

Поля снимка:

- `snapshot_id`
- `type`
- `created_at`
- `config_hash`
- `payload_json`

Снимок также хранится как непрозрачный JSON blob.

### 4.4 `PersistenceConfig`

Есть четыре конфигурационных поля хранения плюс `enabled`:

- `data_dir`
- `journal_subdir`
- `snapshot_subdir`
- `flush_interval_ms`
- `enabled`

Фактически в текущей реализации **реально используется только `enabled`**. Остальные поля пока существуют как заготовка под файловый backend или будущий менеджер flush-политик.

## 5. Контракт `IStorageAdapter`

Интерфейс адаптера минималистичен и симметричен:

1. запись журнала
2. чтение журнала по времени и типу
3. запись снимка
4. загрузка последнего снимка по типу
5. `flush`

Этот контракт достаточно узкий, чтобы поддерживать несколько backend’ов, и достаточно общий, чтобы upper layer не зависел от PostgreSQL или in-memory контейнеров.

Ограничения такого контракта:

- нет удаления / compaction / TTL
- нет пакетной записи
- нет транзакции, объединяющей journal и snapshot
- нет контракта на schema validation payload’а
- нет API для выборки по `correlation_id`, `strategy_id`, `config_hash`

## 6. Как работает `EventJournal`

### 6.1 Основной сценарий append

При вызове `append(...)` журнал делает следующее:

1. берёт текущее время через `std::chrono::system_clock`
2. берёт `mutex_`
3. увеличивает локальный `sequence_counter_`
4. собирает `JournalEntry`
5. вызывает `adapter_->append_journal(entry)`

Смысл mutex’а здесь двойной:

- обеспечить thread-safe инкремент `sequence_counter_`
- сохранить соответствие между порядком выдачи `sequence_id` и порядком отправки записи в backend

### 6.2 Query и flush

`query(...)` и `flush()` тоже выполняются под тем же mutex’ом.

Следствие:

- параллельный `append` и `query` сериализуются
- модуль выбирает простую и безопасную модель, а не максимально конкурентную

### 6.3 Что важно понимать

`EventJournal` сам по себе ничего не знает о трейдинге, ордерах, портфеле или recovery. Он только ставит метаданные и проксирует запись/чтение.

## 7. Как работает `SnapshotStore`

### 7.1 Save

При `save(...)` хранилище снимков:

1. генерирует `snapshot_id` через `atomic<uint64_t>`
2. ставит текущее время через `system_clock`
3. собирает `SnapshotEntry`
4. под `mutex_` вызывает `adapter_->store_snapshot(entry)`

### 7.2 Load

`load_latest(type)` просто делегирует backend’у выборку последнего снимка данного типа.

### 7.3 Особенность генерации ID

В `SnapshotStore` счётчик локален для процесса. После рестарта он начинается заново.

Это нормально для memory backend, но в PostgreSQL backend фактический источник истинного `snapshot_id` уже другой: сама БД.

## 8. Как работает `PersistenceLayer`

`PersistenceLayer` — это тонкий фасад.

Он содержит:

- `adapter_`
- `config_`
- `journal_`
- `snapshots_`

И даёт три вещи:

1. доступ к `journal()`
2. доступ к `snapshots()`
3. `flush()` на оба подслоя

### 8.1 Thread-safety фасада

На уровне фасада нет общего mutex.

Идея такая:

- `EventJournal` thread-safe сам по себе
- `SnapshotStore` thread-safe сам по себе
- фасад не навязывает глобальную блокировку

Это разумно для снижения связности, но означает, что атомарности между journal и snapshots на уровне фасада нет.

### 8.2 Что делает `flush()`

`PersistenceLayer::flush()` вызывает:

1. `journal_.flush()`
2. `snapshots_.flush()`

Если оба используют один и тот же adapter, это означает **двойной вызов `adapter_->flush()`**. Для PostgreSQL это безвредно, для будущего файлового backend это может быть избыточно.

## 9. `MemoryStorageAdapter`

Это тестовый и fallback backend.

### 9.1 Хранение журнала

Журнал хранится как `std::vector<JournalEntry>`.

При `append_journal(...)` запись просто пушится в вектор под mutex.

При `query_journal(...)` выполняется линейный проход по всем записям с фильтрацией:

- по временному диапазону
- по типу, если он задан

Это O(n), но для тестов и fallback-режима этого достаточно.

### 9.2 Хранение снимков

Снимки хранятся в `unordered_map<int, SnapshotEntry>`, где ключом является `SnapshotType`.

Следствие:

- по каждому типу хранится только **последний** снимок
- предыдущая версия снимка того же типа затирается

То есть in-memory backend моделирует поведение `load_latest`, но **не хранит историю снимков**.

### 9.3 Flush

`flush()` — `noop`.

### 9.4 Диагностические методы

Есть два тестовых helper’а:

- `journal_size()`
- `snapshot_count()`

## 10. `PostgresStorageAdapter`

Это полноценный backend персистентности на PostgreSQL.

### 10.1 Схема БД

При создании адаптера автоматически создаются две таблицы:

#### `tb_journal`

Поля:

- `id BIGSERIAL PRIMARY KEY`
- `sequence_id BIGINT`
- `entry_type TEXT`
- `ts_ns BIGINT`
- `correlation_id TEXT`
- `strategy_id TEXT`
- `config_hash TEXT`
- `payload_json TEXT`
- `inserted_at TIMESTAMPTZ DEFAULT now()`

Индексы:

- по `ts_ns`
- по `entry_type`
- по `strategy_id`

#### `tb_snapshots`

Поля:

- `snapshot_id BIGSERIAL PRIMARY KEY`
- `snap_type TEXT`
- `created_at_ns BIGINT`
- `config_hash TEXT`
- `payload_json TEXT`
- `inserted_at TIMESTAMPTZ DEFAULT now()`

Индекс:

- по `snap_type`

### 10.2 Поведение append journal

При `append_journal(...)` адаптер:

1. берёт свой mutex
2. при необходимости переподключается к БД
3. начинает транзакцию `pqxx::work`
4. пишет запись в `tb_journal`
5. коммитит транзакцию
6. только после успешного коммита увеличивает `next_seq_`

Это правильная защита от дыр в sequence при неуспешной записи.

### 10.3 Важная деталь про `sequence_id`

`PostgresStorageAdapter::append_journal(...)` **не использует `entry.sequence_id`, пришедший извне**.

Вместо этого он пишет свой собственный `seq = next_seq_`, инициализированный из `MAX(sequence_id)` в таблице.

Следствие:

- в memory backend источником `sequence_id` является `EventJournal`
- в PostgreSQL backend источником `sequence_id` является сам адаптер

То есть семантика `sequence_id` зависит от backend’а.

### 10.4 Query journal

Выборка идёт по временному диапазону и опциональному типу, сортировка — по `sequence_id ASC`.

Это удобно для replay.

### 10.5 Store / load snapshot

`store_snapshot(...)` всегда делает `INSERT`, то есть PostgreSQL backend хранит **историю снимков**, а не только последний.

`load_latest_snapshot(...)` возвращает запись с максимальным `snapshot_id` для данного `snap_type`.

### 10.6 Важная деталь про `snapshot_id`

Адаптер тоже фактически игнорирует входной `entry.snapshot_id`, потому что реальный `snapshot_id` создаётся в БД как `BIGSERIAL`.

Это вторая backend-dependent семантика ID.

### 10.7 Формат payload

`payload_json` хранится как `TEXT`, а не как `JSONB`.

Это упрощает запись, но означает:

- нет DB-level validation JSON-структуры
- нет индексации по полям JSON
- нет удобных SQL-фильтров по содержимому payload

## 11. `WalWriter`

`WalWriter` — отдельная надстройка над `EventJournal` для write-ahead logging критических действий.

Его идея корректная:

- сначала записать intent в durable journal
- потом выполнить действие
- затем либо `commit`, либо `rollback`

### 11.1 Поддерживаемые типы WAL-записей

- `OrderIntent`
- `OrderSent`
- `OrderCancelled`
- `PositionOpened`
- `PositionClosed`
- `BalanceSync`
- `RecoveryCheckpoint`

### 11.2 Формат записи

Оригинальный payload оборачивается в JSON вида:

```json
{"wal_seq":123,"wal_type":"OrderIntent","committed":false,"data":{...}}
```

Для rollback используется другой envelope:

```json
{"wal_seq":123,"wal_type":"OrderIntent","rollback":true,"reason":"..."}
```

### 11.3 Внутреннее состояние

`WalWriter` хранит `pending_entries_` в памяти процесса.

При `write_intent(...)`:

1. увеличивает `wal_sequence_`
2. формирует `WalEntry`
3. пишет wrapped JSON в `EventJournal`
4. кладёт entry в `pending_entries_`

При `commit(...)` и `rollback(...)`:

1. находит запись в `pending_entries_`
2. пишет commit/rollback envelope в журнал
3. удаляет запись из `pending_entries_`

### 11.4 Что реально умеет `find_uncommitted()`

`find_uncommitted()` проверяет только `pending_entries_` в памяти.

Это значит:

- внутри текущего процесса метод работает
- после рестарта процесса он **не может** восстановить незавершённые WAL-записи из persisted journal

Иначе говоря, WAL сейчас наполовину persistent, наполовину process-local.

### 11.5 Связь WAL с `JournalEntryType`

`WalWriter` маппит свои типы в `JournalEntryType` так:

- order-события → `OrderEvent`
- position/balance → `PortfolioChange`
- checkpoint → `SystemEvent`

Это правильно для маршрутизации, но не решает вопрос replay semantics.

## 12. Реальные runtime-интеграции

## 12.1 Интеграция в `TradingPipeline`

В pipeline есть попытка создать общий PostgreSQL storage:

- если задан `POSTGRES_URL`, вызывается `make_postgres_adapter(...)`
- при ошибке идёт fallback на in-memory

Но здесь есть принципиально важная деталь:

- `PersistenceLayer` не хранится как постоянный член pipeline
- он создаётся локально только для `RecoveryService`
- при этом `recovery_persistence_cfg.enabled = false`

То есть в текущем runtime persistence-инфраструктура **подготовлена**, но историческое snapshot/WAL recovery в pipeline **явно отключено**.

### 12.2 Интеграция в `RecoveryService`

`RecoveryService` использует persistence так:

1. пытается восстановиться из последнего `SnapshotType::Portfolio`
2. если снимок есть, проигрывает `JournalEntryType::PortfolioChange` после времени снимка
3. затем синхронизируется с биржей
4. затем синхронизирует USDT-баланс

Это правильная общая последовательность: snapshot → journal replay → exchange sync.

### 12.3 Что именно восстанавливается из snapshot

На практике `restore_from_snapshot()` восстанавливает только `total_capital` из JSON payload’а.

То есть снимок портфеля сейчас используется **не как полный снапшот портфеля**, а как минимальный источник капитала.

### 12.4 Что именно ожидается в replay journal

`replay_journal_after_snapshot(...)` ожидает raw payload формата:

```json
{"event":"PositionOpened","symbol":"BTCUSDT","amount":0.01,"price":65000.0}
```

Поддерживаются события:

- `PositionOpened`
- `PositionClosed`
- `FeeCharged`
- `CashReserved`
- `CapitalSynced`

Неизвестные события логируются и пропускаются.

## 13. Ключевой архитектурный вывод по WAL и recovery

`WalWriter` и `RecoveryService::replay_journal_after_snapshot(...)` сейчас живут в **двух разных семантических мирах**.

Почему:

1. `WalWriter` пишет payload в WAL-envelope (`wal_seq`, `wal_type`, `committed`, `data`)
2. `RecoveryService` ожидает raw domain-event payload с полем `event`
3. `find_uncommitted()` не умеет перечитывать persisted journal после рестарта

Следствие:

- текущий `WalWriter` полезен как локальный runtime-механизм аудита и intent/commit tracking
- но он **не является готовым crash-recovery источником истины** для существующего `RecoveryService`

Это, пожалуй, самое важное архитектурное наблюдение по модулю.

## 14. Thread-safety и модель времени

### 14.1 Thread-safety

Модуль в целом conservative-thread-safe:

- `EventJournal` защищён mutex’ом
- `SnapshotStore` защищён mutex’ом
- `MemoryStorageAdapter` защищён mutex’ом
- `PostgresStorageAdapter` защищён mutex’ом
- `WalWriter` защищён mutex’ом

Плюсы:

- простая модель
- низкий риск гонок
- предсказуемый порядок записей

Минусы:

- конкуренция append/query ограничена
- нет batch API
- нет lock-free fast path

### 14.2 Источники времени

Здесь есть неоднородность:

- `EventJournal` и `SnapshotStore` используют `std::chrono::system_clock`
- `WalWriter` использует внедрённый `IClock`

Это означает, что timestamps внутри persistence слоя создаются не полностью единообразно.

Для replay это не критично, но для детерминированных тестов и строгой time-source discipline это архитектурный шероховатый участок.

## 15. Тестовое покрытие

Фактически покрыты 11 unit-тестов.

### 15.1 Базовый persistence

Покрыто:

- добавление и запрос journal entries
- фильтрация по типу записи
- сохранение и загрузка snapshot’ов
- автогенерация `sequence_id` в `EventJournal`
- базовый фасад `PersistenceLayer`
- поведение `enabled=false`

### 15.2 WAL

Покрыто:

- `write_intent + commit`
- `write_intent + rollback`
- `find_uncommitted`
- `write_checkpoint`

### 15.3 Что не покрыто явно

- PostgreSQL backend в unit-тестах
- reconnect path для PostgreSQL
- согласованность sequence/snapshot IDs между backend’ами
- recovery replay поверх WAL-envelope
- crash-restart сценарий с восстановлением незакоммиченных WAL-записей
- использование `PersistenceConfig.data_dir/journal_subdir/snapshot_subdir/flush_interval_ms`

По состоянию на 2026-04-09 тесты `346-356` проходят: 11/11.

## 16. Что уже хорошо в модуле

Сильные стороны текущей реализации:

- чистое разделение `journal / snapshots / adapter`
- простой и понятный интерфейс `IStorageAdapter`
- аккуратный in-memory backend для тестов
- PostgreSQL backend со schema bootstrap и базовыми индексами
- thread-safe реализация всех основных компонентов
- наличие отдельного WAL-компонента
- интеграция с recovery по правильной общей схеме snapshot → journal → exchange

## 17. Ограничения и текущие разрывы дизайна

### Ограничение 1. Значительная часть `PersistenceConfig` пока не используется

Поля `data_dir`, `journal_subdir`, `snapshot_subdir`, `flush_interval_ms` сейчас декоративны. Реально используется только `enabled`.

### Ограничение 2. Persistence инфраструктура в pipeline пока не активирована как постоянный runtime-слой

Есть подключение PostgreSQL через `POSTGRES_URL`, но фасад persistence не живёт как долгоживущий компонент pipeline и recovery-path создаётся с `enabled=false`.

### Ограничение 3. `EventJournal` и PostgreSQL backend по-разному трактуют `sequence_id`

Memory backend хранит тот `sequence_id`, который сгенерировал `EventJournal`, а PostgreSQL backend подменяет его своим `next_seq_`.

### Ограничение 4. `SnapshotStore` и PostgreSQL backend по-разному трактуют `snapshot_id`

Memory backend использует ID из `SnapshotStore`, PostgreSQL — свой `BIGSERIAL`.

### Ограничение 5. WAL не доведён до полноценного crash-recovery контура

Он умеет intent/commit/rollback внутри процесса, но не образует законченный persisted replay-путь после рестарта.

### Ограничение 6. Recovery replay ожидает domain events, а не WAL envelopes

Это главный mismatch между двумя подсистемами.

### Ограничение 7. Snapshot portfolio сейчас восстанавливает только капитал

Полный state restore портфеля из snapshot’а пока не реализован.

### Ограничение 8. В модуле нет spot-логики, но в dependent recovery-коде есть spot fallback ветка

Сам `persistence` storage-agnostic и не содержит spot-specific правил.
Но в `RecoveryService::sync_positions_from_exchange(...)` есть отдельная ветка обработки балансов как spot-позиций (`Side::Buy // Спот — всегда long`).

Это уже находится **за пределами persistence**, но важно помнить при анализе интеграций.

## 18. Итоговый вывод

`persistence` в текущем состоянии — это не «готовая завершённая подсистема durability», а хорошо структурированный инфраструктурный каркас с частично подключённым runtime.

Если разделить оценку на два слоя, то картина такая:

### Как storage layer

Модуль сделан добротно:

- понятные контракты
- нормальная композиция
- два backend’а
- есть unit-тесты
- есть база для журнала, снимков и WAL

### Как end-to-end recovery substrate

Подсистема пока неполностью замкнута:

- pipeline не использует persistence как постоянный слой записи
- recovery replay отключён в рабочем pipeline-конструкторе
- WAL и recovery replay ещё не сведены в один формат и один жизненный цикл

Поэтому точный инженерный вывод такой:

- модуль `persistence` реализует хорошие базовые примитивы хранения
- модуль `recovery` уже умеет читать snapshot и journal
- но вся цепочка `runtime writes -> durable storage -> replay after restart` ещё не доведена до полностью активного и единого production-контура

Именно это и есть реальное текущее состояние persistence в проекте.