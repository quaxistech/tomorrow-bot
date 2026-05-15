# Модуль clock — документация

Дата последнего обновления: 2026-04-10

## 1. Назначение модуля

`src/clock` — инфраструктурный модуль времени для `tomorrow-bot`.

Задачи:

1. единый интерфейс получения текущего времени (`IClock`);
2. подмена источника времени в тестах (`TestClock`) и replay/backtest;
3. чёткое разделение wall-time и monotonic-time;
4. портативная inline-функция для монотонного таймера (`steady_now_ns()`).

Для USDT-M futures/scalping-бота время критично:

- timestamp рыночного события;
- время принятия решения;
- возраст данных (freshness);
- cooldown, rate limit, hold duration;
- latency/staleness;
- WAL/journal traceability;
- детерминизм тестов.

## 2. Состав модуля

| Файл | Назначение |
|---|---|
| `clock.hpp` | абстрактный интерфейс `IClock` |
| `wall_clock.hpp/.cpp` | единственная production-реализация (system_clock) |
| `timestamp_utils.hpp` | inline `steady_now_ns()` для монотонного времени |
| `CMakeLists.txt` | сборка статической библиотеки `tb_clock` |

Модуль маленький по объёму, но системный по охвату зависимостей: его линкуют `features`, `market_data`, `normalizer`, `decision`, `risk`, `execution`, `portfolio`, `recovery`, `persistence`, `pipeline`, `supervisor`, `regime`, `uncertainty`, `world_model` и тестовые таргеты.

## 3. Контракт времени

Базовый тип: `tb::Timestamp = StrongType<int64_t, TimestampTag>`.

- наносекунды от Unix-эпохи;
- компилятор отделяет timestamp от других числовых типов;
- нет путаницы `ms` vs `ns` в API;
- перевод в миллисекунды — только на границах с внешними API.

## 4. Интерфейс `IClock`

```cpp
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual Timestamp now() const = 0;
};
```

Контракт: `now()` возвращает наносекунды от Unix-эпохи.

Единственная production-реализация — `WallClock`. В тестах — `TestClock`.

### 4.1. Где используется `IClock`

`IClock` инжектируется в ~20 модулей:

- `market_data`, `normalizer`, `features`, `strategy`, `decision`, `risk`;
- `execution`, `execution_alpha`, `portfolio`, `recovery`, `reconciliation`;
- `resilience`, `persistence`, `regime`, `uncertainty`, `world_model`, `supervisor`.

Паттерны использования:

| Паттерн | Примеры |
|---|---|
| Штамповка событий | `processed_ts`, `computed_at`, `decided_at`, `assessed_at`, `opened_at` |
| Возраст данных | `market_data_age_ns`, `stale feed detection`, `cooldown/expiry` |
| Audit trail | `WalWriter::written_at`, `recovery/reconciliation` timestamps |

## 5. `WallClock`

```cpp
Timestamp WallClock::now() const {
    auto tp = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
    return Timestamp{ns};
}
```

Семантика:

- астрономическое/системное время (UTC);
- пригодно для логов, событий, age-of-data, audit trail;
- не гарантирует монотонность (возможны NTP-скачки);
- **не годится** для точного измерения latency.

Bootstrap: `app_bootstrap.cpp` → `components.clock = clock::create_wall_clock();`

## 6. `steady_now_ns()`

```cpp
[[nodiscard]] inline int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
```

Семантика:

- монотонно возрастающий счётчик;
- не зависит от NTP-коррекции;
- возвращает `int64_t` (не `Timestamp`) — намеренно, чтобы не смешивать с wall-time;
- используется для staleness, интервалов, latency.

Потребители:

- `ml/correlation_monitor.cpp`
- `ml/bayesian_adapter.cpp`
- `ml/thompson_sampler.cpp`
- `ml/liquidation_cascade.cpp`
- `ml/microstructure_fingerprint.cpp`
- `ml/entropy_filter.cpp`
- `pipeline/pipeline_tick_context.hpp` (stage latency)

## 7. Тестовая подмена

В `tests/common/test_mocks.hpp`:

```cpp
class TestClock : public clock::IClock {
public:
    int64_t current_time{1'000'000'000LL};
    [[nodiscard]] Timestamp now() const override { return Timestamp(current_time); }
};
```

Обеспечивает:

- детерминированные unit-тесты;
- ручное управление временем;
- воспроизводимость cooldown, age-gates и decision timestamps.

Используется в ~55 тестовых вызовах.

## 8. Сборка

```cmake
add_library(tb_clock STATIC wall_clock.cpp)
target_link_libraries(tb_clock PUBLIC tb_common)
```

`timestamp_utils.hpp` — header-only, не требует отдельной компоновки.

## 9. Архитектурные решения

### 9.1. Чёткое разделение wall-time и monotonic-time

Модуль сознательно разделяет два класса времени:

- **`IClock::now()` → `Timestamp`** — wall-time для событий, логов, audit trail;
- **`steady_now_ns()` → `int64_t`** — monotonic для latency и staleness.

Разные типы возвращаемых значений (`Timestamp` vs `int64_t`) исключают случайное смешивание семантик.

### 9.2. Централизация time-логики

Весь time-utility код сосредоточен в модуле `clock`:

- нет дублирования monotonic helpers;
- `pipeline_tick_context.hpp` использует `clock::steady_now_ns()`;
- каждый модуль получает время через DI или через `steady_now_ns()`.

## 10. Проведённая очистка (2026-04-10)

| Что | Причина | Действие |
|---|---|---|
| `MonotonicClock` (класс + factory) | 0 внешних вызовов; `steady_clock::time_since_epoch()` ≠ Unix-epoch → нарушение контракта `IClock` | Удалён |
| `now_ns()` | 0 внешних вызовов; дублирует `WallClock::now()` без DI | Удалён |
| `timestamp_to_iso8601()` | 0 внешних вызовов; дублируется в `logging/json_formatter.cpp` (единственный реальный потребитель) | Удалён из clock |
| `timestamp_to_ms()` | 0 внешних вызовов | Удалён |
| `ms_to_timestamp()` | 0 внешних вызовов | Удалён |
| `elapsed_us()` | 0 внешних вызовов | Удалён |
| `pipeline::monotonic_now_ns()` | Дублирование `clock::steady_now_ns()` через raw POSIX `clock_gettime` | Заменён на `clock::steady_now_ns()` |

Результат: 427 тестов проходят, 0 ошибок сборки.
