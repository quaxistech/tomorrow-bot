# `src/resilience` — Устойчивость

## Назначение

Реализация классических паттернов отказоустойчивости: Circuit Breaker, Retry с экспоненциальным backoff, идемпотентность ордеров, операционная защита (auto reduce-risk, reject-rate breaker, state-divergence detection).

## Границы ответственности

* `CircuitBreaker` — Closed→Open→HalfOpen→Closed, потокобезопасный CAS.
* `RetryExecutor` — обёртка над любым callable с retry, jitter, classification.
* `IdempotencyManager` — генерация и дедупликация `client_order_id`.
* `OperationalGuard` — оркестратор инцидентов: reject-rate, consecutive failures, state divergence, venue health.

## Входы / выходы

* CircuitBreaker: события `record_success/failure` → `allow_request()`.
* RetryExecutor: обёртывает HTTP/REST вызовы.
* IdempotencyManager: генерирует ID для каждого ордера; проверяет дубли в окне.
* OperationalGuard: события (success/reject, venue health, position divergence) → `GuardAssessment`.

## Публичные интерфейсы

* `class CircuitBreaker` — `allow_request()`, `record_success()`, `record_failure()`, `state()`, `consecutive_failures()`, `reset()`.
* `class RetryExecutor` — template `execute<F>(name, F)`, `execute_simple(name, F)`, `last_attempts()`.
* `class IdempotencyManager` — `generate_client_order_id`, `is_duplicate`, `mark_sent`, `cleanup_expired`.
* `class OperationalGuard` — `record_order_result/position_check/venue_event`, `assess()`, `operator_halt/resume()`.
* `enum class CircuitState`, `enum class GuardVerdict`, `enum class ErrorClassification`.

## Внутренние компоненты

* `circuit_breaker.hpp/cpp` — атомарный state, half-open attempts.
* `retry_executor.hpp` — header-only template + `execute_simple` в `cpp`.
* `idempotency_manager.hpp/cpp` — sent_ids map + window cleanup.
* `operational_guard.hpp/cpp` — aggregator событий + Prometheus gauges.
* `resilience_types.hpp` — конфиги (`CircuitBreakerConfig`, `RetryConfig`, `IdempotencyConfig`).

## Зависимости

* `clock/IClock`, `logging/ILogger`, `metrics/IMetricsRegistry`.
* `common/types.hpp` (для `Symbol`, `OrderId`, `StrategyId`).

## Потоки данных

* REST вызов → `RetryExecutor::execute<F>` → проверка `breaker.allow_request()` → `operation()` → classification → retry с backoff (sleep) → final response.
* Создание ордера → `IdempotencyManager::generate_client_order_id` → `mark_sent` → submit. Дублирующий intent → `is_duplicate` true → `Err`.
* Каждый order_result/divergence/venue event → `OperationalGuard::record_*` → пересчёт verdict → trade-flow modifications.

## Race conditions

* `CircuitBreaker` использует CAS на atomic state (linearizable transitions).
* `IdempotencyManager` под `mutex_`.
* `OperationalGuard` под `mutex_`.
* `RetryExecutor::last_attempts_` под `mutex_` — read/write.

## Ошибки проектирования

* **D-res-1 (HIGH).** `RetryExecutor::execute<F>` использует `std::this_thread::sleep_for(delay)` — блокирует вызывающий поток. Если RetryExecutor вызывается из hot-path (как в `BitgetRestClient`), мьютексы pipeline удерживаются весь период retry (до 3000 мс backoff). См. § 7 в корне.
* **D-res-2 (MEDIUM).** `CircuitBreaker::half_open_attempts_` — `mutable atomic<int>`, но reset в `reset()` без CAS на `state_` — теоретически race при concurrent `allow_request` + `reset`.
* **D-res-3 (MEDIUM).** `IdempotencyManager::generate_client_order_id` использует `sequence_` атомарно, но не учитывает `clock_offset_ms` биржи — при подмене serial порядка ID на бирже могут оказаться вне ожидаемого окна.
* **D-res-4 (LOW).** `OperationalGuard::halt_reason_` и `degraded_reason_` — не обнуляются при `operator_resume()` (требует верификации).

## Контракты

### `CircuitBreaker::allow_request()`

* **Pre.** Никаких.
* **Post.** Возвращён `bool`. Если `state == Open ∧ now - last_failure < cooldown` → false. Если `Open ∧ cooldown истёк` → state→HalfOpen, true.
* **Invariant.** Допустимы переходы только: Closed↔Open, Open→HalfOpen, HalfOpen→Closed (на success), HalfOpen→Open (на failure).

### `IdempotencyManager::generate_client_order_id(symbol, side, strategy_id)`

* **Pre.** Все аргументы непустые.
* **Post.** Возвращён string длины ≤ 40 (Bitget лимит для clientOid). Уникален в окне `dedup_window_ms`.
* **Invariant.** Не может конфликтовать с ID, который ещё в `sent_ids_`.

## Производственные риски

* **R-res-1.** При длительных сетевых проблемах `RetryExecutor` блокирует hot-path → пропуск тиков → backlog WS. Mitigation: `RetryExecutor` должен исполняться на dedicated thread.
* **R-res-2.** `CircuitBreaker` cooldown слишком короткий → флаппинг.
* **R-res-3.** `IdempotencyManager::cleanup_expired` должен вызываться периодически — иначе map растёт. Кто это делает? — TBD верифицировать.

## Рекомендации

1. Async retry: `RetryExecutor::execute_async<F>` возвращает `std::future` или `co_await` (C++20 coro).
2. Гарантировать `cleanup_expired` через scheduled task в `Supervisor`.
3. Тестовый набор для `CircuitBreaker`: TSAN под нагрузкой 100K req/s с 30% failure rate.
4. Метрика `circuit_breaker_state` и `idempotency_duplicates_total` — экспортировать в Prometheus с тегом `name`.
