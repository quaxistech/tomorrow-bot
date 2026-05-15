# `src/strategy_allocator` — Аллокация весов между стратегиями

## Назначение

Распределение веса между стратегиями в зависимости от текущего режима рынка / world model. Backwards-compat: фактически в системе одна стратегия (`StrategyEngine`), но интерфейс позволяет multi-strategy.

## Границы ответственности

* Per-tick расчёт weights = `f(regime, world_state, performance_stats)`.
* Возврат `AllocationResult{weights map, normalized}`.

## Публичные интерфейсы

* `class IStrategyAllocator`:
  * `allocate(regime, world, available_strategies) → AllocationResult`.
* Реализация (RuleBased / Static / Adaptive — зависит от файла).
* `AllocationResult` — `unordered_map<StrategyId, double weight>`.

## Внутренние компоненты

* `strategy_allocator.hpp/cpp`.
* `allocation_types.hpp` — DTO.

## Зависимости

* `strategy/strategy_types.hpp` (StrategyId).
* `regime`, `world_model`.
* `logging`.

## Потоки данных

`TradingPipeline → strategy_allocator_.allocate(regime, world, ...) → AllocationResult` → передаётся в `decision_engine_.aggregate(...)`.

## Race conditions

Зависит от реализации; если есть state — mutex.

## Ошибки проектирования

* **D-sa-1 (LOW).** При single-strategy конфигурации модуль возвращает `{strategy_id: 1.0}` — overhead. Можно скипнуть decision pipeline.

## Контракты

### `allocate(regime, world, strategies) → AllocationResult`

* **Pre.** Все snapshot'ы валидные. `strategies` непуст.
* **Post.** Сумма весов ≈ 1.0 (после normalization).
* **Invariant.** Все веса ≥ 0.

## Производственные риски

* **R-sa-1.** При multi-strategy: некорректные веса → плохой decision.

## Рекомендации

1. Удалить модуль при single-strategy архитектуре (или сохранить как простой identity-allocator) — снизит сложность.
2. Тест: суммa весов = 1.0 для всех комбинаций (regime × world × strategies).
