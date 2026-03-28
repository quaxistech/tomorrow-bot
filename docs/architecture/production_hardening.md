# Production Hardening v0.4 — Архитектура операционной надёжности

## Обзор

Версия 0.4 вводит слой операционной надёжности, приближающий систему к уровню
профессиональных спотовых торговых систем. Основной фокус: **state integrity first, alpha second**.

```
                            ┌─────────────────────────────────────┐
                            │          Supervisor v2              │
                            │  ┌─ Symbol Lock Registry ─────────┐│
                            │  ├─ Kill Switch Broadcast ────────┤│
                            │  ├─ Global Position Limits ───────┤│
                            │  └─ Shutdown Timeout Controller ──┘│
                            └──────────┬──────────────────────────┘
                                       │ coordinates
            ┌──────────────────────────┼──────────────────────────┐
            ▼                          ▼                          ▼
   ┌─────────────────┐     ┌─────────────────────┐    ┌──────────────────┐
   │ ReconciliationEng│     │   RecoveryService   │    │ Resilience Layer │
   │                  │     │                     │    │                  │
   │ • Order recon    │     │ • Snapshot restore  │    │ • CircuitBreaker │
   │ • Position recon │     │ • WAL replay        │    │ • RetryExecutor  │
   │ • Balance recon  │     │ • Exchange sync     │    │ • IdempotencyMgr │
   └────────┬─────────┘     └──────────┬──────────┘    └────────┬─────────┘
            │                          │                         │
            ▼                          ▼                         ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │                        Execution Engine v2                           │
   │  ┌─ Partial Fill Policy ──────────────────────────────────────────┐  │
   │  ├─ Fill Event Tracking (per-fill price, qty, fee) ───────────────┤  │
   │  ├─ Order Timeout & Auto-cancel ──────────────────────────────────┤  │
   │  ├─ Slippage Tracking (expected vs actual) ───────────────────────┤  │
   │  └─ Order FSM v2 (force_transition, time_in_state, timeout) ──────┘  │
   └──────────────────────────────┬───────────────────────────────────────┘
                                  │
                                  ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │                    WAL Writer (Write-Ahead Log)                      │
   │  write_intent(OrderIntent) → execute() → commit(seq) / rollback()   │
   │                                                                      │
   │  Паттерн: запись ДО действия, подтверждение ПОСЛЕ                    │
   │  Storage: PostgreSQL через EventJournal + IStorageAdapter            │
   └──────────────────────────────────────────────────────────────────────┘
```

## Новые модули

### 1. Reconciliation Engine (`src/reconciliation/`)

**Цель**: обнаружение и разрешение расхождений между внутренним состоянием и биржей.

**Файлы**:
- `reconciliation_types.hpp` — типы: MismatchType, ResolutionAction, MismatchRecord, ReconciliationResult, ExchangeOrderInfo, ExchangePositionInfo, ReconciliationConfig
- `reconciliation_engine.hpp` — IExchangeQueryService (интерфейс запросов к бирже) + ReconciliationEngine
- `reconciliation_engine.cpp` — реализация

**Потоки данных**:
```
Startup:
  Exchange API ─→ get_open_orders() ─→ compare with local orders_ ─→ detect mismatches
  Exchange API ─→ get_account_balances() ─→ compare with portfolio ─→ detect mismatches
  Auto-resolve (if config allows) or escalate to operator
```

**Классификация расхождений** (7 типов):
| Тип | Описание | Авто-разрешение |
|-----|----------|-----------------|
| OrderExistsOnlyOnExchange | Ордер на бирже, нет локально | SyncFromExchange |
| OrderExistsOnlyLocally | Локальный ордер, нет на бирже | UpdateLocalState |
| StateMismatch | Состояние расходится | SyncFromExchange |
| QuantityMismatch | filled_qty расходится | SyncFromExchange |
| PositionExistsOnlyOnExchange | Позиция на бирже, нет локально | AlertOperator |
| PositionExistsOnlyLocally | Локальная позиция, нет на бирже | AlertOperator |
| BalanceMismatch | Баланс за пределами допуска | AlertOperator |

### 2. Recovery Service (`src/recovery/`)

**Цель**: восстановление полного состояния после crash/restart.

**Файлы**:
- `recovery_types.hpp` — RecoveryMode, RecoveryStatus, RecoveredPosition, RecoveredOrder, RecoveryResult, RecoveryConfig
- `recovery_service.hpp/cpp` — RecoveryService

**Стратегия восстановления** (3 уровня):
1. **Snapshot restore**: загрузка последнего снимка портфеля из PostgreSQL
2. **WAL replay**: воспроизведение журнала событий после временной метки снимка
3. **Exchange sync**: финальная сверка с биржей для устранения расхождений

### 3. Resilience Layer (`src/resilience/`)

**Цель**: защита от каскадных отказов, идемпотентность, retry discipline.

**Файлы**:
- `resilience_types.hpp` — RetryConfig, CircuitBreakerConfig, CircuitState, ErrorClassification, ExecutionAttempt, IdempotencyConfig
- `circuit_breaker.hpp/cpp` — lock-free CircuitBreaker
- `retry_executor.hpp/cpp` — template RetryExecutor с exponential backoff + jitter
- `idempotency_manager.hpp/cpp` — генерация ClientOrderId + дедупликация

**Circuit Breaker FSM**:
```
Closed ──(failure_threshold exceeded)──→ Open
Open ──(recovery_timeout expired)──→ HalfOpen
HalfOpen ──(success)──→ Closed
HalfOpen ──(failure)──→ Open
```

**Retry Backoff**: `delay = min(base_delay × 2^attempt + random(0, jitter_factor × delay), max_delay)`

**ClientOrderId формат**: `{prefix}_{strategy}_{symbol}_{side}_{timestamp_ms}_{sequence}`

### 4. WAL Writer (`src/persistence/wal_writer.hpp/cpp`)

**Цель**: crash-safe запись критических действий.

**Паттерн**: Write-Ahead Logging
```
1. write_intent(OrderIntent, payload) → wal_seq
2. execute_order()
3a. commit(wal_seq)       // действие выполнено
3b. rollback(wal_seq)     // действие не выполнено
```

**Recovery**: `find_uncommitted()` возвращает все записи без commit/rollback → система решает: повторить или отменить.

### 5. Production Guard (`src/security/production_guard.hpp/cpp`)

**Цель**: предотвращение случайного запуска с реальными деньгами.

**Проверки**:
- Paper/Shadow/Testnet — всегда разрешены
- Production — требуется `TOMORROW_BOT_PRODUCTION_CONFIRM` env variable
- Детекция production API URL (отсутствие «testnet» в base URL)
- Логирование всех проверок для аудита

## Расширения существующих модулей

### Supervisor v2

**Новые возможности**:
- **Symbol Lock Registry**: `try_lock_symbol()` / `unlock_symbol()` — эксклюзивный доступ к символу для одного pipeline
- **Kill Switch Broadcast**: `register_kill_switch_listener()` / `activate_global_kill_switch()` — мгновенное уведомление всех зарегистрированных pipeline
- **Global Position Limits**: `register_open_position()` / `can_open_position()` — атомарная проверка глобальных лимитов
- **Shutdown Timeout**: `set_shutdown_timeout()` — контролируемое завершение с force-continue при превышении

### Execution Engine v2

**Новые возможности**:
- **PartialFillPolicy**: `WaitForFull` / `CancelRemaining` / `AllowPartial`
- **FillEvent tracking**: каждый partial fill записывается отдельно (qty, price, fee, trade_id)
- **Order timeout**: `cancel_timed_out_orders(max_open_duration_ms)`
- **Slippage tracking**: `realized_slippage = avg_fill_price - expected_fill_price`

### Order FSM v2

- `force_transition()`: обход валидации для recovery
- `time_in_current_state_ms()`: для timeout detection
- `last_transition_time()`: для аудита

### Error Codes

13 новых кодов ошибок:
- Reconciliation: `ReconciliationFailed`, `ReconciliationMismatch`
- Recovery: `RecoveryFailed`, `RecoveryIncomplete`
- Resilience: `CircuitBreakerOpen`, `RetryExhausted`, `IdempotencyDuplicate`
- WAL: `WalWriteFailed`, `WalRecoveryFailed`
- Coordination: `SymbolLockFailed`, `GlobalLimitExceeded`
- Safety: `ProductionGuardFailed`

## Метрики (Prometheus)

| Метрика | Тип | Описание |
|---------|-----|----------|
| `reconciliation_mismatches_total` | counter | Общее кол-во обнаруженных расхождений |
| `reconciliation_duration_ms` | gauge | Время последней reconciliation |
| `reconciliation_auto_resolved_total` | counter | Кол-во авто-разрешённых расхождений |
| `recovery_duration_ms` | gauge | Время recovery при startup |
| `recovery_positions_count` | gauge | Кол-во восстановленных позиций |
| `wal_writes_total` | counter | Записи в WAL |
| `wal_commits_total` | counter | Подтверждённые записи WAL |
| `wal_rollbacks_total` | counter | Откаченные записи WAL |
| `order_fills_total` | counter | Кол-во fill events |

## Тесты

28 новых тестов:
- `tests/unit/reconciliation/` — 6 тестов (matching, mismatches, auto-resolve)
- `tests/unit/recovery/` — 4 теста (clean start, positions, dust, balance)
- `tests/unit/resilience/` — 10 тестов (circuit breaker, retry, idempotency)
- `tests/unit/persistence/test_wal_writer.cpp` — 4 теста (intent/commit/rollback/uncommitted)
- `tests/unit/security/test_production_guard.cpp` — 4 теста (paper/testnet/production modes)

## Зависимости

```
tb_reconciliation → tb_common, tb_execution, tb_portfolio, tb_logging, tb_clock, tb_metrics
tb_recovery → tb_common, tb_reconciliation, tb_portfolio, tb_persistence, tb_logging, tb_clock, tb_metrics
tb_resilience → tb_common, tb_logging, tb_clock, tb_metrics
tb_persistence (updated) → tb_common, pqxx, pq (PostgreSQL)
tb_supervisor (updated) → tb_common, tb_health, tb_logging, tb_metrics, tb_clock, tb_governance
```
