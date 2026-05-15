# `src/clock` — Часы

## Назначение

Абстракция времени для детерминированного тестирования и единообразного источника timestamp'ов в системе.

## Границы ответственности

* Получение текущего времени (wall + monotonic).
* Утилиты конвертации (`ms ↔ ns ↔ Timestamp ↔ system_clock::time_point`).
* Возможность подмены часов в тестах через `IClock`.

Не отвечает за: clock skew с биржей (это в `BitgetRestClient::clock_offset_ms`), таймауты политик (это в `RiskEngine`/`ExecutionEngine`).

## Входы / выходы

* Вход: системное время через `std::chrono`.
* Выход: `Timestamp` (наносекунды от Unix-эпохи).

## Публичные интерфейсы

* `class IClock` — абстракция:
  * `Timestamp now() const`.
  * (опц.) `int64_t monotonic_ns() const`.
* `class WallClock : IClock` — production-реализация на `std::chrono::system_clock`.
* `std::shared_ptr<IClock> create_wall_clock()`.
* `timestamp_utils.hpp` — конвертеры.

## Внутренние компоненты

* `clock.hpp` — интерфейс `IClock`.
* `wall_clock.hpp/cpp` — `WallClock`.
* `timestamp_utils.hpp` — `to_ms`, `to_ns`, `to_seconds`, `format_iso8601`.

## Зависимости

* `common/types.hpp` (`Timestamp`).
* STL `<chrono>`.

## Потоки данных

`now()` — read-only, потокобезопасно.

## Race conditions

Отсутствуют. `system_clock::now()` thread-safe.

## Ошибки проектирования

* **D-clock-1 (MEDIUM).** Нет монотонных часов в публичном интерфейсе `IClock`. Отдельные модули используют `std::chrono::steady_clock::now()` напрямую (см. `OrderFSM::time_in_current_state_ms`). Это нарушает принцип единого источника времени и затрудняет deterministic replay в тестах.
* **D-clock-2 (LOW).** `WallClock` не учитывает `clock_offset_ms` от биржи. Если локальные часы расходятся с серверными более чем на лимит Bitget, подпись REST запроса rejected.

## Контракты

### `IClock::now()`

* **Pre.** Никаких.
* **Post.** Возвращён `Timestamp t` с `t.get() ≥ 0`.
* **Invariant.** Монотонно неубывающий (для `WallClock` нарушается при NTP-step). Для тестового clock контролируется тестом.

## Производственные риски

* **R-clock-1.** NTP step ≥ N сек может нарушить Inv-D3 (`opened_at ≤ updated_at`). Защита: использовать `steady_clock` для интервалов и `system_clock` только для timestamping. Уже частично реализовано в `OrderFSM`.

## Рекомендации

1. Добавить `IClock::monotonic_ns()` обязательным методом.
2. Унифицировать использование: все intervals через monotonic, все labels через wall.
3. Добавить тестовый `MockClock` с возможностью set/advance — упростит regression-тесты `RiskEngine` и `LeverageEngine` (EMA smoothing).
