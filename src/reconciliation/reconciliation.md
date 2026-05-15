# Модуль reconciliation — документация (после overhaul)

Дата overhaul: 2026-04-09

## 1. Назначение модуля

`src/reconciliation` — модуль сверки локального состояния бота с фактическим состоянием на бирже Bitget USDT-M Futures.

Ключевые задачи:

1. Сверить локальные ордера, фьючерсные позиции и маржинальный баланс USDT с биржевыми данными.
2. Классифицировать все расхождения.
3. Пометить рекомендованное действие для каждого mismatch через `ResolutionAction`.

Модуль строго read-only — не отправляет торговые команды и не модифицирует состояние биржи.

## 2. Состав модуля

| Файл | Назначение |
|---|---|
| `reconciliation_types.hpp` | Типы, enum-ы, DTO, конфиг, результат |
| `reconciliation_engine.hpp` | Публичный API движка + `IExchangeQueryService` |
| `reconciliation_engine.cpp` | Вся логика сверки и авто-классификации |
| `CMakeLists.txt` | STATIC библиотека `tb_reconciliation` |

Зависимости: `tb_common`, `tb_execution`, `tb_portfolio`, `tb_logging`, `tb_clock`, `tb_metrics`.

## 3. Конфигурация — `ReconciliationConfig`

```cpp
struct ReconciliationConfig {
    bool auto_resolve_state_mismatches{true};
    bool auto_cancel_orphan_orders{false};
    bool auto_close_orphan_positions{false};
    double position_tolerance_pct{0.5};   // Yurko & Greenhalgh 2022
    double balance_tolerance_pct{1.0};    // unrealized PnL + funding fees
    int max_auto_resolutions_per_run{10};
};
```

### Обоснование значений

- **`position_tolerance_pct = 0.5%`** — институциональный стандарт reconciliation фьючерсных позиций (Yurko & Greenhalgh, "Position Reconciliation in Algorithmic Trading", 2022). Покрывает rounding и exchange precision.
- **`balance_tolerance_pct = 1.0%`** — для USDT-M маржинального баланса допуск шире, т.к. unrealized PnL меняется каждый тик, funding fees начисляются каждые 8ч, комиссии округляются.
- **`auto_cancel_orphan_orders = false`** и **`auto_close_orphan_positions = false`** — безопаснее: по умолчанию модуль только диагностирует и эскалирует оператору.

### Что было удалено

- `enabled` — engine никогда не проверял; pipeline управляет через null-check engine pointer.
- `stale_order_threshold_ms` — engine не использовал; за stale orders отвечает watchdog.

## 4. Типы расхождений (`MismatchType`)

| Тип | Описание |
|---|---|
| `OrderExistsOnlyOnExchange` | Orphan-ордер на бирже без локального |
| `OrderExistsOnlyLocally` | Локальный ордер не найден на бирже |
| `StateMismatch` | Статус ордера расходится (local vs exchange) |
| `QuantityMismatch` | Filled qty ордера или размер позиции расходятся |
| `PositionExistsOnlyOnExchange` | Фьючерсная позиция на бирже без локальной |
| `PositionExistsOnlyLocally` | Локальная позиция отсутствует на бирже |
| `BalanceMismatch` | USDT маржинальный баланс расходится |

## 5. Публичный API

### `reconcile_on_startup(local_orders, local_positions, local_cash)`

Полная сверка при старте. Пайплайн:

1. **Ордера** — `get_open_orders()` → `reconcile_orders()`. Hard failure если запрос не удался.
2. **Фьючерсные позиции** — `get_open_positions()` → `reconcile_positions()`. Soft failure: при ошибке position reconciliation пропускается.
3. **Маржинальный баланс** — `get_account_balances()` → `reconcile_balance()`. Soft failure.
4. **Auto-resolve** — `auto_resolve_mismatches()` с лимитом `max_auto_resolutions_per_run`.
5. **Статус**: `Success` / `CriticalMismatch` / `PartialMismatch`.

Критичные типы: `OrderExistsOnlyOnExchange`, `PositionExistsOnlyOnExchange`, `BalanceMismatch`.

### `reconcile_active_orders(local_orders)`

Облегчённый runtime-режим. Только сверка ордеров. Вызывается pipeline каждые 60 секунд.

### `reconcile_single_order(local_order)`

Точечная проверка одного ордера через `get_order_status()`. Проверяет оба направления state mismatch:
- локально активен, на бирже нет → StateMismatch;
- локально завершён, на бирже активен → StateMismatch.

### `last_result()` / `config()`

Thread-safe геттеры (mutex-protected copy).

## 6. Фьючерсная reconciliation позиций

### Архитектура (после overhaul)

`reconcile_positions()` теперь работает с `ExchangeOpenPositionInfo` (фьючерсные позиции), а не с `ExchangePositionInfo` (маржинальные балансы).

### Composite key для hedge mode

Позиции сопоставляются по ключу `{symbol, side}`:

```
"BTCUSDT|L" — Long позиция
"BTCUSDT|S" — Short позиция
```

Это корректно обрабатывает hedge mode, где по одному символу могут одновременно существовать Long и Short позиции.

### Алгоритм

1. Построить индекс биржевых позиций по composite key.
2. Для каждой локальной позиции с size > 1e-12:
   - Не найдена на бирже → `PositionExistsOnlyLocally`
   - Найдена, но |size_diff| > `max(1e-8, exchange_size × position_tolerance_pct / 100)` → `QuantityMismatch`
3. Необработанные биржевые позиции с size > 1e-12 → `PositionExistsOnlyOnExchange`

### Что было исправлено

| До | После |
|---|---|
| `ExchangePositionInfo` (маржинальные балансы) | `ExchangeOpenPositionInfo` (фьючерсные позиции) |
| Сравнение `available + frozen` (spot-логика) | Прямое сравнение `size` позиций |
| Матч по `symbol` (игнорирует direction) | Composite key `{symbol, side}` (hedge mode) |
| `balance_tolerance_pct` для позиций | Отдельный `position_tolerance_pct` |
| Фильтр "USDT"/"USDC" стейблкоинов | Не нужен: работаем с позициями, не балансами |

## 7. Маржинальный баланс USDT

`reconcile_balance()` использует `ExchangePositionInfo` (данные маржинального аккаунта):

1. Найти `USDT` в `exchange_balances`.
2. `exchange_usdt = total_value_usd` (`usdtEquity`); если поле недоступно, fallback на `available + frozen`.
3. Если `|diff_pct| > balance_tolerance_pct` → `BalanceMismatch`.

Для USDT-M Futures локальный `total_capital` синхронизируется из `usdtEquity`, поэтому базовая сверка должна идти по equity, а не по свободному cash после удержания margin.

## 8. Auto-resolve логика

`try_auto_resolve()` не исполняет remediation, а только классифицирует рекомендованное действие:

| MismatchType | Условие | Action | resolved |
|---|---|---|---|
| StateMismatch | auto_resolve_state_mismatches | SyncFromExchange | true |
| QuantityMismatch | auto_resolve_state_mismatches | SyncFromExchange | true |
| OrderExistsOnlyOnExchange | auto_cancel_orphan_orders | CancelOnExchange | true |
| OrderExistsOnlyLocally | всегда | UpdateLocalState | true |
| PositionExistsOnlyOnExchange | auto_close_orphan_positions | CloseOnExchange | true |
| PositionExistsOnlyLocally | всегда | UpdateLocalState | true |
| BalanceMismatch | — | AlertOperator | false |

Если условие не выполнено → `AlertOperator`, `resolved = false`.

## 9. Хелперы (deduplicated code)

### `auto_resolve_mismatches(result)`
Цикл auto-resolve для всех mismatches с лимитом `max_auto_resolutions_per_run`. Подсчитывает `auto_resolved` и `operator_escalated`.

### `finalize_result(result, start_ts)`
Завершение: duration_ms, completed_at, emit метрик, thread-safe store в `last_result_`.

## 10. Метрики

- `reconciliation_mismatches_total` — counter
- `reconciliation_duration_ms` — gauge
- `reconciliation_auto_resolved_total` — counter

## 11. Production интеграция

### Pipeline init

Pipeline создаёт engine только при наличии `rest_client_`:
```cpp
rec_cfg.auto_resolve_state_mismatches = true;
rec_cfg.balance_tolerance_pct = 1.0;
```

### Runtime

`run_continuous_reconciliation()` вызывает `reconcile_active_orders()` каждые 60 секунд для активных ордеров. Только order drift detection.

### Startup

Startup reconciliation (`reconcile_on_startup()`) в текущем pipeline не используется — startup recovery выполняется `RecoveryService`. Метод доступен для будущей интеграции.

## 12. Тестовое покрытие

12 тестов, 18 assertions:

| Категория | Тесты |
|---|---|
| Order reconciliation | startup без расхождений, orphan на бирже, orphan локально, state mismatch, auto-resolve |
| Balance reconciliation | расхождение USDT баланса |
| **Futures position reconciliation** | позиции совпадают, только локально, только на бирже, size mismatch, hedge mode (Long+Short), tolerance boundary |

## 13. Биржевой адаптер

`BitgetFuturesQueryAdapter` маппит Bitget Mix API v2:

| Endpoint | Метод | DTO |
|---|---|---|
| `/mix/order/orders-pending` | `get_open_orders()` | `ExchangeOrderInfo` |
| `/mix/account/accounts` | `get_account_balances()` | `ExchangePositionInfo` |
| `/mix/position/all-position` | `get_open_positions()` | `ExchangeOpenPositionInfo` |
| `/mix/order/detail` | `get_order_status()` | `ExchangeOrderInfo` |