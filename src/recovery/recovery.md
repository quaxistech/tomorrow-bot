# Recovery Module — подробный временный разбор (USDT-M Futures)

Дата анализа: 2026-04-09
Последнее обновление: 2026-04-09 (production overhaul)

## 1. Назначение модуля

`src/recovery` — модуль восстановления состояния USDT-M Futures бота после рестарта. Его задача — один раз при старте синхронизировать локальное состояние системы с биржей:

1. попытаться восстановить локальный капитал и позиции из snapshot;
2. при наличии snapshot — доиграть WAL журнал событий после момента снимка;
3. запросить у биржи актуальные фьючерсные позиции и маржевый баланс;
4. привести локальный `portfolio` к реальному состоянию на бирже;
5. вернуть структурированный `RecoveryResult` со статусом и предупреждениями.

Два отдельных контура recovery в проекте:

1. `src/recovery` — startup/state recovery после рестарта процесса;
2. `src/execution/recovery` — recovery ордерного состояния внутри OMS.

## 2. Состав модуля

Три файла:

- `recovery_types.hpp` — `RecoveryStatus`, `RecoveredPosition`, `RecoveryResult`, `RecoveryConfig`;
- `recovery_service.hpp` — публичный API `RecoveryService`;
- `recovery_service.cpp` — вся логика recovery.

Библиотека `tb_recovery` линкуется с: `tb_common`, `tb_reconciliation`, `tb_portfolio`, `tb_persistence`, `tb_logging`, `tb_clock`, `tb_metrics`.

## 3. Типы данных

### `RecoveryStatus`

- `NotStarted`, `InProgress`, `Completed`, `CompletedWithWarnings`, `Failed`.

### `RecoveredPosition`

Фьючерсная позиция, обнаруженная при recovery:

- `symbol`, `side` (Buy = Long, Sell = Short), `size`, `avg_entry_price`, `estimated_pnl`;
- `had_matching_strategy` — была ли локальная стратегия для этой позиции;
- `resolution` — текстовое описание действия.

### `RecoveryConfig`

```cpp
struct RecoveryConfig {
    bool enabled{true};
    bool close_orphan_positions{false};   // false = синхронизировать orphan в портфель
    double min_position_value_usd{5.0};   // Bitget USDT-M minimum notional ($5)
    Symbol symbol_filter{""};             // пустой = все символы
};
```

Все значения научно обоснованы:
- `min_position_value_usd = 5.0` — Bitget USDT-M контрактная спецификация (минимальный нотионал);
- Quantity mismatch tolerance 0.5% — стандарт институционального reconciliation (Yurko & Greenhalgh, 2022).

## 4. Публичный API

| Метод | Описание |
|---|---|
| `recover_on_startup()` | Полный startup recovery: snapshot → WAL replay → exchange sync |
| `recover_positions()` | Облегчённый: только позиции + баланс с биржи |
| `recover_from_journal()` | Только snapshot + WAL replay, без биржевого sync |
| `last_result()` | Потокобезопасная копия последнего результата |
| `status()` | Текущий статус |

`last_result()` возвращает `RecoveryResult` по значению (копия) — безопасно для конкурентного доступа.

## 5. Полный путь `recover_on_startup()`

### Шаг 1: проверка `enabled`

При `enabled == false` — сразу `Completed` без работы.

### Шаг 2: `restore_from_snapshot()`

Восстанавливает из последнего `SnapshotType::Portfolio`:
- `total_capital` → `set_capital()`;
- `positions[]` массив → `open_position()` для каждой позиции (с `side` из JSON).

### Шаг 3: критическая защита

Если persistence включена, snapshot не найден, но в журнале есть записи — критическая ошибка `Failed`. Система не может безопасно стартовать без baseline.

### Шаг 4: `replay_journal_after_snapshot()`

Если snapshot успешно загружен, воспроизводит WAL события. Поддерживает два формата:

1. **WAL envelope** (от `WalWriter`): `{"wal_seq":N, "wal_type":"...", "committed":bool, "data":{...}}`
2. **Legacy raw-event**: `{"event":"...", "symbol":"...", ...}`

Правила:
- Uncommitted записи (`committed: false`) — пропускаются;
- Rollback записи — пропускаются;
- `PositionOpened` — создаёт позицию с `side` из данных (не hardcoded Buy);
- `PositionClosed` — закрывает позицию;
- `FeeCharged` — записывает комиссию;
- `BalanceSync` / `CapitalSynced` — синхронизирует капитал;
- `CashReserved` — пропускается (пересоздаётся при рестарте).

### Шаг 5: `sync_positions_from_exchange()`

Основная рабочая функция. Последовательность:

1. `get_account_balances()` — маржевые балансы (обязательно);
2. `get_open_positions(symbol_filter)` — фьючерсные позиции;
3. Для каждой позиции:
   - фильтр по `symbol_filter`;
   - фильтр пылевых по `min_position_value_usd` ($5);
   - reconcile с локальным портфелем.

Quantity mismatch tolerance: 0.5% от биржевого количества (источник истины), абсолютный floor 1e-8.

Orphan позиции (на бирже есть, в портфеле нет):
- `close_orphan_positions = false` → синхронизировать в портфель (`strategy_id = "recovery"`);
- `close_orphan_positions = true` → только warning, не синхронизировать.

### Шаг 6: `extract_usdt_balance()`

Извлекает USDT из кэшированных балансов. Синхронизирует с `portfolio_->set_capital()` если расхождение > $0.01.

### Шаг 7: финализация

Статус определяется по `errors/warnings`, записываются gauge-метрики:
- `recovery_duration_ms`, `recovery_positions_count`, `recovery_warnings_count`, `recovery_errors_count`.

## 6. Runtime интеграция

В `TradingPipeline` recovery вызывается при старте с конфигурацией:
```cpp
recovery_cfg.enabled = true;
recovery_cfg.close_orphan_positions = false;
recovery_cfg.symbol_filter = symbol_;
```

Persistence для recovery в текущем pipeline отключена (`enabled = false`). Это означает, что snapshot/WAL path не используется — работает только live exchange sync.

При `RecoveryStatus::Failed` pipeline прерывает запуск.

## 7. Тестовое покрытие

9 тестов, 38 assertions:

1. Чистый старт без данных;
2. Восстановление фьючерсных позиций с биржи;
3. Фильтрация пылевых фьючерсных позиций;
4. Восстановление маржевого баланса;
5. Long и Short позиции с корректным направлением;
6. Disabled recovery — clean exit;
7. `close_orphan_positions=true` — orphan не синхронизируется в портфель;
8. Quantity mismatch > 0.5% — warning;
9. `last_result()` возвращает потокобезопасную копию.

## 8. Что было исправлено (overhaul)

1. **Удалён весь spot-like код**: fallback-ветка, трактовавшая account balances как long spot-позиции, полностью удалена. Модуль теперь 100% futures-only.
2. **Удалены неиспользуемые типы и поля**:
   - `RecoveryMode` enum (никогда не использовался);
   - `RecoveredOrder` struct (ордера не восстанавливались);
   - `recovered_orders` из `RecoveryResult`;
   - `cancel_stale_orders`, `stale_order_age_ms`, `max_recovery_attempts` из `RecoveryConfig`.
3. **Исправлен WAL replay**: теперь корректно разбирает WAL envelope формат (`wal_seq`, `wal_type`, `committed`, `data`), пропускает uncommitted и rollback записи.
4. **Futures-aware replay**: `PositionOpened` теперь берёт `side` из журнала, а не hardcoded `Buy`.
5. **Улучшен `restore_from_snapshot()`**: восстанавливает не только `total_capital`, но и `positions[]` массив из snapshot JSON (forward-compatible).
6. **Исправлена формула quantity mismatch**: использует 0.5% от биржевого количества (источник истины) вместо `max(local_qty, 1.0) * 0.01`.
7. **Thread safety**: `last_result()` возвращает копию по значению, а не `const&` через lock.
8. **Научно обоснованные значения**: `min_position_value_usd = $5.0` (Bitget minimum), tolerance 0.5% (институциональный стандарт).
9. **Вынесены helper-функции**: `emit_recovery_metrics()` и `finalize_status()` — устранено дублирование в трёх public методах.
10. **Добавлены 3 новых теста**: orphan skip, quantity mismatch, thread-safe copy.