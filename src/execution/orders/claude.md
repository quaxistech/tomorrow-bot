# `src/execution/orders` — OrderRegistry и client_order_id

## Назначение

Single source of truth для всех ордеров: хранение, поиск, FSM-переходы, дедупликация intent'ов, идемпотентность fill'ов, очистка терминальных.

## Границы ответственности

* CRUD `OrderRecord` (по `OrderId` и по `exchange_order_id`).
* FSM transitions (через `OrderFSM` хранимый внутри).
* Idempotency fill: `is_fill_applied`/`mark_fill_applied`.
* Intent deduplication.
* Active/terminal counters.
* `client_order_id` helpers (генерация, валидация формата).

## Публичные интерфейсы

* `class OrderRegistry`:
  * Конструктор `(IClock, ILogger, ExecutionConfig)`.
  * `register_order(OrderRecord)`.
  * `get_order(OrderId) → optional<OrderRecord>`.
  * `get_order_by_exchange_id(OrderId) → optional<OrderRecord>`.
  * `update_order(OrderRecord)`.
  * `active_orders()`, `orders_for_symbol(Symbol)`.
  * `transition(id, new_state, reason) → bool`.
  * `force_transition(id, new_state, reason)`.
  * `fsm_state(id) → optional<OrderState>`.
  * `time_in_state_ms(id) → optional<int64_t>`.
  * `is_fill_applied(id) → bool`, `mark_fill_applied(id)`.
* `client_order_id.hpp` — `make_client_order_id(...)`, format validators.

## Внутренние компоненты

* `order_registry.hpp/cpp`.
* `client_order_id.hpp` (header-only helpers).

## Зависимости

* `execution/order_types.hpp`, `execution/order_fsm.hpp`, `execution/execution_config.hpp`.
* `clock`, `logging`.

## Потоки данных

* `register_order` → создаётся FSM в New, добавляется в `unordered_map<OrderId, ...>`. Если есть `client_order_id`, регистрируется в дедуп-маппинг.
* `transition` → проксируется в `OrderFSM::transition`.
* `mark_fill_applied` → добавляет в set; `is_fill_applied` query.

## Race conditions

`mutex_` под все операции.

## Ошибки проектирования

* **D-orr-1 (LOW).** `unordered_map<OrderId, OrderRecord>` копирует `OrderRecord` каждый раз. Для больших books — overhead. Mitigation: `unordered_map<OrderId, unique_ptr<OrderRecord>>`.
* **D-orr-2 (LOW).** `force_transition` доступен публично; нет invariant что вызывается только из `RecoveryManager`. Совершенно легко misuse.

## Контракты

См. § 9.3 в корневом `claude.md`.

### `is_fill_applied(id)`

* **Pre.** `id` валиден.
* **Post.** Возвращён boolean.

### `mark_fill_applied(id)`

* **Pre.** Запись существует.
* **Post.** Последующий `is_fill_applied(id) = true`.
* **Invariant.** Idempotent: повторный вызов — no-op.

## Производственные риски

* **R-orr-1.** `force_transition` без `reason` затрудняет post-mortem.

## Рекомендации

1. Сделать `force_transition` `friend`-only с `RecoveryManager` или передавать sentinel-токен.
2. Метрика `order_registry_size` (gauge) и `order_registry_terminal_total{state}`.
3. Тест: properties FSM transitions через property-based testing.
