# Модуль resilience — подробный разбор

Дата: 2026-04-10 (обновлено после production-grade overhaul)

## 1. Назначение модуля

`src/resilience` — набор building block-ов отказоустойчивости для сетевых и ордерных операций USDT-M futures.

Модуль решает три задачи:

1. ограничивать каскадные отказы через `CircuitBreaker`;
2. повторно выполнять временно неуспешные операции через `RetryExecutor`;
3. предотвращать повторную отправку ордеров через `IdempotencyManager`.

## 2. Состав модуля

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | сборка библиотеки `tb_resilience` |
| `resilience_types.hpp` | общие типы, enum-ы и конфигурации |
| `circuit_breaker.hpp/.cpp` | автомат защиты от каскадных отказов |
| `retry_executor.hpp/.cpp` | retry-исполнитель с backoff, jitter и breaker |
| `idempotency_manager.hpp/.cpp` | генерация idempotent ClientOrderId и дедупликация |

## 3. Зависимости

`tb_resilience` → `tb_common`, `tb_logging`, `tb_clock`, `tb_metrics`

## 4. Конфигурации и научное обоснование дефолтов

### 4.1. RetryConfig

| Поле | Дефолт | Обоснование |
|---|---|---|
| `max_retries` | 3 | AWS SDK / Google Cloud SDK default (Google SRE Book §22) |
| `base_delay_ms` | 100 | AWS/GCP SDKs initial retry delay: 100ms |
| `max_delay_ms` | 5000 | Потолок для скальпинга USDT-M futures (5s max, time-sensitive) |
| `jitter_factor` | 0.5 | "Equal Jitter" подход (Marc Brooker, AWS Architecture Blog 2015) |
| `rate_limit_backoff_multiplier` | 3 | Stripe best practice: 2-4x multiplier для 429 |

### 4.2. CircuitBreakerConfig

| Поле | Дефолт | Обоснование |
|---|---|---|
| `failure_threshold` | 5 | Fowler Circuit Breaker pattern: 5-10 consecutive |
| `recovery_timeout_ms` | 30000 | Microsoft Polly / Resilience4j default: 30s |
| `half_open_max_attempts` | 3 | Resilience4j `permittedNumberOfCallsInHalfOpenState` |

### 4.3. IdempotencyConfig

| Поле | Дефолт | Обоснование |
|---|---|---|
| `client_id_prefix` | "tb" | Namespace для ClientOrderId |
| `dedup_window_ms` | 300000 | 5 мин — стандартное TTL exchange order (Bitget USDT-M) |

## 5. Архитектура компонентов

### 5.1. CircuitBreaker

Потокобезопасный автомат состояний (Closed → Open → HalfOpen → Closed).

Ключевые свойства после overhaul:

- **CAS для Open→HalfOpen**: `compare_exchange_strong` предотвращает thundering herd — только один поток выполняет переход, остальные пробуют как HalfOpen.
- **Injected clock**: опциональный `clock::IClock` для детерминированного тестирования; fallback на `steady_clock`.
- **Метрики**: `circuit_breaker_transitions_total{name, from, to}` — каждый переход состояния.
- **half_open_max_attempts**: первый запрос (transition) считается за probe — `half_open_max_attempts=3` означает ровно 3 пробных запроса.

### 5.2. RetryExecutor

Объединяет retry policy, error classification, circuit breaker, backoff, логирование и метрики.

Ключевые свойства после overhaul:

- **Единая реализация**: `execute_simple()` делегирует в шаблонный `execute()` через `SimpleResponse` адаптер. Устранено дублирование ~100 строк кода.
- **Корректное управление mutex**: `unique_lock` отпускается на время `sleep_for()` в обоих методах.
- **Потокобезопасный `last_attempts()`**: возвращает копию вектора с захватом mutex.
- **Конфигурируемый rate limit backoff**: `rate_limit_backoff_multiplier` вместо hardcoded x3.
- **Bitget API awareness**: `classify_error()` распознаёт Bitget error codes в body (43011 — rate limit, 40014/40016/40017 — auth failures).

### 5.3. IdempotencyManager

Генерация уникальных ClientOrderId и дедупликация.

Формат: `{prefix}_{strategy}_{symbol}_{B|S}_{timestamp_ms}_{seq}`

Метрики: `idempotency_ids_generated_total`, `idempotency_duplicates_detected_total`.

## 6. Error Classification

| HTTP Status | Bitget Code | Класс | Поведение |
|---|---|---|---|
| 0 | — | Transient | Retry с backoff |
| 429 | — | RateLimit | Retry с x3 backoff |
| 401, 403 | — | AuthFailure | НЕ retry, немедленный stop |
| 5xx | — | Transient | Retry с backoff |
| 4xx | — | Permanent | НЕ retry |
| 200 | 43011 | RateLimit | Retry с x3 backoff |
| 200 | 40014, 40016, 40017 | AuthFailure | НЕ retry |

## 7. Удалённый код

- `RetryConfig::retry_on_timeout` — never used, dead field
- `IdempotencyConfig::enabled` — never checked, dead field
- `execute_simple()` полная реализация (~80 строк) — заменена 5-строчным делегированием в `execute()`

## 8. Исправленные баги

1. **execute_simple() lock-during-sleep**: `std::lock_guard` держал mutex во время `sleep_for()`, блокируя все конкурентные операции на время backoff. Устранено через делегирование в `execute()`, который использует `unique_lock` с unlock/relock.
2. **last_attempts() data race**: возвращал `const&` без захвата mutex. Теперь возвращает потокобезопасную копию.
3. **Open→HalfOpen thundering herd**: `state_.store()` позволял нескольким потокам одновременно выполнить переход. Заменён на `compare_exchange_strong`.

## 9. Тестовое покрытие

- 21 test case, 88 assertions (было 10/37)
- Новые тесты: HalfOpen failure→Open, half_open_max_attempts limit, CB reset, CB metrics, permanent error early-stop, auth failure early-stop, template execute, classify_error HTTP, classify_error Bitget codes, ClientOrderId format, multi mark_sent/cleanup.
- CircuitBreaker тесты используют injected clock — детерминированные, без `sleep_for`.

## 10. Фьючерсный контекст

Модуль не содержит spot-логики. Вся error classification ориентирована на Bitget USDT-M futures API.
