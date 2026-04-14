# Модуль logging — подробный разбор

Временный рабочий документ.

Дата: 2026-04-10

## 1. Назначение модуля

`src/logging` — это инфраструктурный модуль наблюдаемости, который даёт всей системе единый API для записи диагностических событий.

Его задача не в том, чтобы "печатать строки в консоль", а в том, чтобы:

1. принимать события от runtime-модулей через общий интерфейс `ILogger`;
2. нормализовать их в `LogEvent`;
3. обогащать события thread-local контекстом;
4. форматировать их в text или JSON;
5. отправлять их в один или несколько sink'ов.

Архитектурно это не бизнес-модуль, а поперечная инфраструктура. Через него проходят сообщения bootstrap, exchange adapters, supervisor, reconciliation, strategy engine, normalizer, indicators, pipeline и других подсистем.

## 2. Состав модуля

В каталоге находятся 7 файлов:

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | сборка библиотеки `tb_logging` |
| `logger.hpp/.cpp` | `ILogger`, `ConsoleLogger`, `FileLogger`, `CompositeLogger`, фабрики |
| `json_formatter.hpp/.cpp` | JSON-сериализация `LogEvent`, экранирование строк, ISO-8601 timestamp |
| `log_event.hpp` | модель события лога и enum `LogLevel` |
| `log_context.hpp` | thread-local контекст и `ScopedCorrelationId` |

Это обычная `STATIC` библиотека без внешних logging-framework зависимостей.

## 3. Сборка и зависимости

`tb_logging` собирается из двух `.cpp`:

- `logger.cpp`
- `json_formatter.cpp`

Зависимости:

- `tb_common`

Практически это означает:

- модуль использует базовые strong types из `common/types.hpp`, в первую очередь `Timestamp`;
- никаких внешних библиотек вроде `spdlog`, `fmt`, `Boost.Log`, `glog` или `Poco` здесь нет;
- вся логика форматирования и маршрутизации реализована локально.

По `CMake` модуль декларирует `cxx_std_20`, что согласуется с использованием `std::format` в коде и отсутствием здесь прямой зависимости на `std::expected`.

## 4. Общий runtime-flow: как модуль живёт в системе

Реальный стартовый путь выглядит так:

1. `config` загружает секцию `logging` из YAML.
2. `AppBootstrap::initialize()` читает:
   - `logging.level`
   - `logging.structured_json`
   - `logging.output_path`
3. Bootstrap маппит строковый уровень (`trace/debug/info/warn/error/critical`) в enum `logging::LogLevel`.
4. Всегда создаётся `ConsoleLogger`.
5. Если `output_path != "-"`, bootstrap пытается создать `FileLogger`.
6. Если файл открыт успешно, оба sink'а объединяются в `CompositeLogger`.
7. Получившийся `std::shared_ptr<ILogger>` сохраняется в `AppComponents` и потом прокидывается во все крупные runtime-подсистемы.

То есть logging-модуль в системе инициализируется рано, сразу после загрузки конфига и до запуска production guard, scanner, pipeline и exchange adapters.

## 5. `log_event.hpp` — базовая модель события

`LogEvent` — центральная единица модуля. В ней лежат:

- `level`
- `timestamp`
- `component`
- `message`
- `correlation_id`
- `fields`

### 5.1. `LogLevel`

Уровни объявлены как enum:

- `Trace`
- `Debug`
- `Info`
- `Warn`
- `Error`
- `Critical`

Вспомогательные функции:

- `log_level_value()` возвращает числовой приоритет для сравнения;
- `to_string()` возвращает uppercase-представление (`TRACE`, `DEBUG`, `INFO`, ...).

Это позволяет выполнять cheap-фильтрацию по уровню без строковых сравнений внутри hot path.

### 5.2. `Timestamp`

В `LogEvent` используется strong type `Timestamp` из `common/types.hpp`.

В событии хранится значение в наносекундах от Unix epoch.

Важно: внутренняя точность события выше, чем у JSON-представления. При форматировании в ISO-8601 модуль отбрасывает наносекунды до миллисекунд.

## 6. `ILogger` — контракт модуля

`ILogger` задаёт три обязательных метода:

- `log(LogEvent event)`
- `set_level(LogLevel level)`
- `get_level() const`

Кроме них, в базовом классе реализованы удобные методы:

- `trace()`
- `debug()`
- `info()`
- `warn()`
- `error()`
- `critical()`

Они не виртуальные и все сводятся к одному паттерну:

1. собрать `LogEvent` через `make_event()`;
2. вызвать виртуальный `log()` конкретной реализации.

Это хорошая архитектурная точка: формат события строится централизованно, а sink-реализации отвечают только за фильтрацию и вывод.

## 7. `ILogger::make_event()` — место, где лог реально формируется

Это самый важный метод модуля с точки зрения поведения.

Он делает три ключевые вещи.

### 7.1. Ставит timestamp

Событие получает `std::chrono::system_clock::now()` и сохраняется в наносекундах.

### 7.2. Подмешивает `LogContext`

Из thread-local контекста читаются:

- `correlation_id`
- произвольные поля `fields`

Правило merge такое:

- если в переданных `fields` нет `correlation_id`, а в `LogContext` он есть, то он добавляется в `fields`;
- все поля из `LogContext.fields()` копируются только если такой ключ ещё не задан явно вызывающей стороной.

Следствие: явные поля лог-вызова имеют приоритет над thread-local контекстом.

### 7.3. Формирует top-level `correlation_id`

Кроме вставки в `fields`, `make_event()` отдельно записывает `ctx.correlation_id()` в поле `event.correlation_id`.

Это важный нюанс: корреляционный идентификатор живёт сразу в двух местах:

- в top-level поле `correlation_id`
- потенциально внутри `fields["correlation_id"]`

Для text formatter это частично компенсировано: он пропускает поле `correlation_id` в цикле по `fields`.

Для JSON formatter такой компенсации нет, поэтому в JSON возможна дубликация `correlation_id`:

- как отдельного top-level поля;
- как поля внутри объекта `fields`.

Это не ломает логирование, но важно для анализа downstream consumers.

## 8. `log_context.hpp` — thread-local контекст

`LogContext` задуман как per-thread контекст трассировки.

Он хранит:

- `correlation_id_`
- `component_`
- `fields_`

И живёт как `thread_local` singleton через `LogContext::current()`.

### 8.1. Что он умеет

Можно:

- установить correlation id;
- установить component;
- добавить/удалить поля;
- очистить только поля;
- сбросить контекст полностью.

### 8.2. Что реально используется

По коду проекта `LogContext` как feature почти не эксплуатируется.

Практически:

- `ILogger::make_event()` читает его всегда;
- но в runtime-коде почти нет мест, где кто-то сознательно вызывает `set_correlation_id()`, `set_field()` или `ScopedCorrelationId`.

То есть механизм есть, но система в основном живёт без него.

### 8.3. `component_` сейчас архитектурно лишний

Хотя `LogContext` хранит `component_`, он никак не участвует в построении `LogEvent`.

`make_event()` всегда берёт `component` из явного аргумента метода `info()/warn()/error()`.

Следствие:

- `LogContext::set_component()` существует;
- `LogContext::component()` существует;
- но в реальной логике сборки события это поле не используется.

То есть `component_` в текущем состоянии — латентный, но не активный feature.

### 8.4. `ScopedCorrelationId`

`ScopedCorrelationId` — RAII-обёртка:

1. при создании сохраняет предыдущий correlation id;
2. ставит новый;
3. в деструкторе восстанавливает старый.

Идея правильная для request tracing, но по проекту этот инструмент почти не используется.

## 9. `ConsoleLogger` — базовый синхронный sink

`ConsoleLogger` хранит:

- `min_level_` как `std::atomic<int>`;
- `json_format_` как флаг выбора formatter branch.

### 9.1. Фильтрация

Каждое событие сравнивается с минимальным уровнем.

Если `event.level < min_level`, событие просто отбрасывается.

### 9.2. Форматирование

Затем вызывается `format_event(event, json_format_)`:

- JSON branch использует `format_as_json()`;
- text branch собирает строку вручную через `ostringstream`.

### 9.3. Разделение stdout / stderr

Фактическая реализация делает так:

- `Error` и `Critical` идут в `stderr`;
- `Trace`, `Debug`, `Info`, `Warn` идут в `stdout`.

Здесь есть важное расхождение между комментарием и кодом.

В комментарии к `ConsoleLogger` написано, что `Warn/Error/Critical -> stderr`, но реализация реально направляет в `stderr` только `Error` и выше.

То есть поведение модуля определяется кодом, а не комментарием: `Warn` остаётся в `stdout`.

### 9.4. Ограничения консольного sink-а

`ConsoleLogger` синхронный и не использует mutex.

Это означает:

- нет очереди;
- нет batching;
- нет гарантии line-atomicity между потоками;
- при высокой конкуренции вывод может визуально перемешиваться.

Для development/debug это обычно допустимо, для production-high-throughput системы это уже компромисс.

## 10. Text formatting — как выглядит обычный лог

В text branch строка строится так:

```text
[   LEVEL] [component] message [cid=...] key=value key2=value2
```

Особенности:

- уровень выравнивается через `setw(8)`;
- `component` обязателен и передаётся вызывающей стороной;
- `correlation_id` печатается отдельным блоком `[cid=...]`;
- поле `correlation_id` в цикле по `fields` специально пропускается, чтобы не дублироваться.

### 10.1. Ограничение text branch

В текстовом формате нет timestamp.

Это очень важный факт.

`LogEvent.timestamp` создаётся всегда, но обычный text output его не показывает. Следовательно:

- при `structured_json=false` логи теряют временную метку на уровне вывода;
- диагностика по времени становится слабее;
- JSON branch для production объективно полезнее.

## 11. `json_formatter.*` — structured logging слой

JSON formatter отвечает за три вещи:

- escape строк;
- формат времени;
- сериализацию всего `LogEvent` в одну строку JSON.

### 11.1. `json_escape()`

Функция экранирует:

- `"`
- `\\`
- `\n`
- `\r`
- `\t`
- управляющие символы `< 0x20` через `\uXXXX`

То есть для JSON-строк модуль ведёт себя корректно и не полагается на внешний serializer.

### 11.2. `timestamp_to_iso8601()`

Функция:

1. переводит наносекунды в секунды и миллисекунды;
2. строит UTC-время через `gmtime_r` / `gmtime_s`;
3. форматирует как ISO-8601:

```text
YYYY-MM-DDTHH:MM:SS.mmmZ
```

То есть в JSON время выводится в UTC и в читаемом ingestion-friendly формате.

### 11.3. `format_as_json()`

JSON содержит:

- `timestamp`
- `level`
- `component`
- `message`
- `correlation_id` (если не пуст)
- `fields` (если не пуст)

Формат one-line, без pretty-print. Это правильно для ELK/Loki/Fluent Bit pipelines.

### 11.4. Важный нюанс JSON branch

`format_as_json()` печатает все поля из `event.fields` без исключения.

Из-за этого возможна дубликация `correlation_id`, если он уже был подмешан в `fields` на стадии `make_event()`.

Текстовый formatter это поле фильтрует, JSON formatter — нет.

## 12. `FileLogger` — append-only файловый sink

`FileLogger` открывает файл один раз в конструкторе и пишет в него в append mode.

### 12.1. Поведение конструктора

Он делает следующее:

1. строит `std::filesystem::path`;
2. если у пути есть parent directory, пытается создать её через `create_directories()`;
3. открывает файл с флагами `std::ios::out | std::ios::app`.

Это означает:

- путь может быть как относительным, так и абсолютным;
- старый лог не перетирается;
- запись всегда идёт в конец файла.

### 12.2. Потокобезопасность

В `log()` используется `std::lock_guard` на `mutex_`.

Это делает запись в файл более надёжной, чем вывод в консоль:

- строка пишется под mutex;
- интерливинг между потоками для файла практически исключён.

### 12.3. Ограничения файлового sink-а

Есть несколько важных ограничений:

- нет log rotation;
- нет reopen policy;
- нет size limit;
- нет retention policy;
- нет backpressure;
- нет явной обработки I/O ошибок после открытия.

То есть это простой append-only sink, а не полнофункциональная production logging subsystem.

## 13. `CompositeLogger` — fan-out поверх нескольких sink'ов

`CompositeLogger` просто хранит `vector<shared_ptr<ILogger>>` и последовательно вызывает каждый sink.

### 13.1. Что он даёт

Он нужен bootstrap-пути, где лог должен одновременно идти:

- в консоль;
- в файл.

### 13.2. Как он работает

- `log()` последовательно вызывает `sink->log(event)` для каждого sink;
- `set_level()` прокидывает новый уровень во все sink'и;
- `get_level()` возвращает уровень первого не-null sink'а.

### 13.3. Практические последствия

- fan-out синхронный;
- медленный sink тормозит весь лог-вызов;
- один и тот же `LogEvent` копируется в downstream calls по value semantics.

То есть `CompositeLogger` прост и надёжен, но не оптимизирован для очень частого логирования.

## 14. Реальная интеграция с приложением

### 14.1. Bootstrap

`AppBootstrap` — главная точка инициализации модуля.

Именно здесь из config policy получается реальный runtime logger.

Если файл лога не открылся, bootstrap не падает. Он просто пишет warning и продолжает жить на одном `ConsoleLogger`.

Это важное production-решение: логирование деградирует, но не ломает весь старт приложения.

### 14.2. Dependency injection по системе

Дальше `std::shared_ptr<ILogger>` пробрасывается в разные подсистемы как зависимость конструктора.

Из явных потребителей по проекту:

- `reconciliation`
- `supervisor`
- `exchange/bitget`
- `execution_alpha`
- `normalizer`
- `indicators`
- `strategy`
- `market_data`
- `resilience`
- `ml`
- `pipeline`
- `recovery`
- `uncertainty`

То есть logging-модуль фактически является одним из базовых DI-сервисов проекта.

### 14.3. Стиль использования

Типичный паттерн в коде такой:

```cpp
logger_->warn("Reconciliation", "Расхождение состояния ордера", {
    {"order_id", id},
    {"symbol", symbol}
});
```

Следовательно, практическая модель модуля — это structured event logging с явным `component` и дополнительными key/value полями.

## 15. Тестовое покрытие

У самого logging-модуля dedicated unit tests нет.

Это очень важное наблюдение.

### 15.1. Что есть

Есть `TestLogger` в `tests/common/test_mocks.hpp`.

Это минимальная no-op реализация `ILogger`, которая позволяет тестировать другие модули без реального I/O.

Также многие integration/unit tests создают обычный `ConsoleLogger(LogLevel::Error)`, чтобы:

- подавить шум логов;
- передать валидный logger dependency.

### 15.2. Чего нет

Не покрыты напрямую:

- `json_escape()`;
- `timestamp_to_iso8601()`;
- `format_as_json()`;
- фильтрация по уровням;
- разруливание stdout/stderr;
- `FileLogger`;
- `CompositeLogger`;
- `LogContext`;
- `ScopedCorrelationId`.

То есть logging сейчас активно используется системой, но почти не тестируется как самостоятельный модуль.

## 16. Важные инженерные наблюдения

### 16.1. Модуль синхронный

И `ConsoleLogger`, и `FileLogger`, и `CompositeLogger` работают синхронно.

Это значит:

- каждый лог-вызов форматирует строку сразу;
- каждый sink пишет её сразу;
- поток выполнения блокируется на I/O.

Для умеренной нагрузки это нормально.

Для HFT/scalping production-path это уже не идеальный вариант, особенно если логирование частое и включён `debug`.

### 16.2. Модуль сам это признаёт

В `logger.hpp` прямо сказано, что для production желательно использовать асинхронный структурированный логгер.

То есть автор модуля осознанно оставил здесь минималистичную synchronous implementation, а не считает её финальной вершиной эволюции.

### 16.3. `LogContext` — latent feature

Хотя инфраструктура correlation/tracing уже реализована, по проекту она почти не используется.

Следовательно, сегодня logging-модуль реально работает прежде всего как:

- level filtering
- text/json formatting
- sink fan-out

а не как полноценный distributed tracing / contextual logging layer.

### 16.4. `component` из контекста не работает

Контекст умеет хранить `component`, но `LogEvent` строится из явно переданного аргумента функции `info()/warn()/error()`.

Это означает, что автоматической component propagation нет.

### 16.5. Text output теряет время

Text formatter не выводит timestamp.

Если кто-то работает с `structured_json=false`, то лог становится существенно менее полезным для post-mortem анализа.

### 16.6. JSON output может дублировать `correlation_id`

Это не фатальная ошибка, но важная особенность текущей реализации.

### 16.7. Порядок полей не детерминирован

Поскольку `fields` хранится в `unordered_map`, порядок ключей в text и JSON formatter branch не гарантирован.

Для машинного ingestion это нормально.
Для snapshot-тестов и byte-for-byte сравнения это было бы неудобно.

### 16.8. Уровень логирования почти не меняется в runtime

Хотя `set_level()` реализован во всех sink'ах, по проекту почти не видно реального runtime reconfiguration.

То есть это capability есть, но operationally модуль в основном живёт с уровнем, заданным на bootstrap.

## 17. Краткий итог

`src/logging` — это компактный инфраструктурный модуль логирования с тремя основными ролями:

1. привести сообщения разных подсистем к общей модели `LogEvent`;
2. форматировать события в text или JSON;
3. отправлять их в console/file/composite sink.

На практике модуль уже полезен и хорошо интегрирован в систему:

- интерфейс `ILogger` чистый;
- dependency injection проведён последовательно;
- JSON formatter достаточно аккуратный;
- файл и консоль поддерживаются без внешних библиотек.

Но при глубоком разборе видно, что это всё ещё минималистичная production-infrastructure, а не полноценная logging-platform:

- логирование синхронное;
- собственных тестов почти нет;
- `LogContext` и `ScopedCorrelationId` в основном спят;
- text formatter не показывает timestamp;
- есть поведенческие нюансы вроде дублирования `correlation_id` в JSON и расхождения комментария с реальным `stdout/stderr` routing.

Именно поэтому модуль стоит понимать как рабочий базовый observability layer системы, но не как завершённую high-throughput logging subsystem.