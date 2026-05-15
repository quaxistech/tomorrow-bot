# `src/strategy/state` — State machine + setup models

## Назначение

DTO моделей сетапа/позиции и реализация state machine для жизненного цикла сетапа в рамках одного символа.

## Границы ответственности

* `Setup` — DTO предложения входа (entry zone, stop, target, conviction).
* `SetupValidationResult` — DTO результата проверки.
* `PositionManagementResult` — действия с открытой позицией (hold/reduce/exit).
* `StrategyStateMachine` — переходы между `SymbolState`.

## Публичные интерфейсы

* `class StrategyStateMachine`:
  * `current() → SymbolState`.
  * `transition(SymbolState new_state, reason)` → bool.
  * `force_set(SymbolState)` для recovery.
* `struct Setup`, `struct SetupValidationResult`, `struct PositionManagementResult` (DTO).
* `enum class SymbolState`.

## Внутренние компоненты

* `strategy_state.hpp/cpp` — state machine.
* `setup_models.hpp` — DTO.

## Зависимости

* `strategy/strategy_types.hpp` (для `SymbolState`, `SetupType`).
* `common`.

## Потоки данных

`StrategyEngine` хранит instance `StrategyStateMachine` per symbol; каждый `evaluate()` может вызвать `transition()`.

## Race conditions

`StrategyStateMachine` под mutex'ом владельца (`StrategyEngine::mutex_`).

## Ошибки проектирования

* **D-stst-1 (LOW).** Допустимая матрица переходов hard-coded в реализации; нет explicit DSL/таблицы. Изменение порядка состояний требует править условия.

## Контракты

### `transition(new_state, reason) → bool`

* **Pre.** Никаких.
* **Post.** Если переход допустим → `current_ = new_state`, return true. Иначе → false без изменения.
* **Invariant.** Допустимые переходы согласно ТЗ §12.

### `force_set(state)`

* **Pre.** Используется только в recovery / тестах.
* **Post.** `current_ = state` без проверки.

## Производственные риски

* **R-stst-1.** `force_set` без логирования может вызвать необъяснимое поведение. Рекомендуется обязательный `reason`.

## Рекомендации

1. Tabular state machine (массив разрешённых переходов).
2. Логирование каждого перехода (level=Debug).
3. Property-based тест: `forall transitions: corner-case checks`.
