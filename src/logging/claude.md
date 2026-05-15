# `src/logging` — Структурированный логгер

## Назначение

Единый интерфейс структурированного логирования (key-value контекст + JSON-сериализация). Все компоненты системы логируют через `ILogger`. Никаких `std::cout`/`fmt::print` в production-коде.

## Границы ответственности

* Уровни: `Trace`, `Debug`, `Info`, `Warn`, `Error`, `Critical`.
* Контекстные поля: `{{key, value}, ...}` для каждого события.
* Sink-ы: console, file, composite (broadcast).
* Форматтеры: JSON (структурный) и human-readable (опционально).

Не отвечает за: ротацию файлов (предполагается logrotate / external), асинхронную запись (sync writes).

## Входы / выходы

* Вход: `LogEvent` (компонент, сообщение, контекст, level, timestamp).
* Выход: текст в stdout / файл (через configured sink).

## Публичные интерфейсы

* `class ILogger`:
  * `void trace/debug/info/warn/error/critical(component, message, context = {})`.
  * `bool is_enabled(LogLevel) const`.
* `class LogContext` — type-erased ключ-значение.
* `enum class LogLevel`.
* Фабрики:
  * `create_console_logger(level, json_fmt)`.
  * `create_file_logger(path, level, json_fmt)` — может вернуть `nullptr` при ошибке.
  * `create_composite_logger(sinks)` — broadcast.

## Внутренние компоненты

* `logger.hpp` — интерфейс + фабрики.
* `log_event.hpp` — `LogEvent` структура.
* `log_context.hpp` — типобезопасные kv-пары.
* `json_formatter.hpp` — сериализация в JSON-line.

## Зависимости

* `clock/IClock` — для timestamp каждого события.
* STL stream + filesystem.
* Никаких внешних логгер-библиотек.

## Потоки данных

`logger->info(...)` → `LogEvent` → `LogContext` сериализуется в строку → write в sink. Все sink-ы синхронные.

## Race conditions

* `FileSink` использует mutex для последовательной записи в один файл.
* `CompositeLogger` итерирует список sinks без mutex (предполагается, что sinks фиксируются на конструкции).

## Ошибки проектирования

* **D-log-1 (HIGH).** Sync writes в hot-path. На каждый тик может быть 5-15 log записей; при медленном диске блокирует pipeline. Mitigation: bounded queue + dedicated logger thread.
* **D-log-2 (MEDIUM).** Composite logger не lockfree. Если хоть один sink медленный — все ждут.
* **D-log-3 (MEDIUM).** Production-guardrail в `AppBootstrap` повышает level до Info, но между моментом создания логгера и проверки уже могут пройти trace-сообщения.
* **D-log-4 (LOW).** Нет встроенного rate-limiting (per-component throttling). Bug-spam при повторяющихся ошибках.

## Контракты

### `ILogger::warn(component, message, context)`

* **Pre.** `component != "" ∧ message != ""`.
* **Post.** Если `is_enabled(LogLevel::Warn)` — событие записано в sink (sync). Иначе noop.
* **Invariant.** Контракт thread-safety: метод callable из любого потока.

## Производственные риски

* **R-log-1.** При FATAL уровне (Critical) можно ожидать перенаправление в дополнительный канал (Slack/SMS). Сейчас только лог. Mitigation: интеграция с alerting через `OperationalGuard`.

## Рекомендации

1. Async logger с backpressure (drop при переполнении + counter в metrics).
2. Per-component rate limiting.
3. JSON sink с буферизацией (line-buffered fallback).
4. Тесты: проверить, что redaction (`security/redaction.hpp`) автоматически применяется к полям `api_key`, `secret` и т.п. до сериализации.
