# `src/strategy/setups` — Setup detector & validator

## Назначение

Детекция и валидация скальпинговых сетапов (4 типа) согласно §10, §13-15 ТЗ. Stateless-классы, оперируют `StrategyContext` и возвращают `optional<Setup>` либо `SetupValidationResult`.

## Границы ответственности

* `SetupDetector::detect(ctx, id, now)` — попытка обнаружить любой из 4 типов сетапа.
* `SetupValidator::validate(setup, ctx, now)` — проверка, что сетап остаётся валидным.
* `SetupValidator::can_confirm(setup, ctx, now)` — готов ли к входу.

## Публичные интерфейсы

* `class SetupDetector` — методы:
  * `detect(ctx, id, now) → optional<Setup>`.
  * Внутренние: `detect_momentum`, `detect_retest`, `detect_pullback`, `detect_rejection`.
* `class SetupValidator`:
  * `validate(setup, ctx, now) → SetupValidationResult`.
  * `can_confirm(setup, ctx, now) → bool`.

## Внутренние компоненты

* `setup_lifecycle.hpp/cpp` — обе реализации.

## Зависимости

* `strategy/strategy_types.hpp` — `Setup`, `SetupValidationResult`, `StrategyContext`.
* `strategy/strategy_config.hpp` — `ScalpStrategyConfig`.
* `features/feature_snapshot.hpp`.

## Потоки данных

`StrategyEngine::handle_pre_entry` → `setup_detector_.detect(ctx, ...)` → если есть `Setup`, переход state machine → периодический `setup_validator_.validate(setup, ctx, ...)` пока не `can_confirm` или `validation.canceled`.

## Race conditions

Stateless. Конкурентный вызов безопасен.

## Ошибки проектирования

* **D-stsu-1 (MEDIUM).** Логика детекции каждого типа сетапа — самостоятельный набор условий по `FeatureSnapshot`; нет общей абстракции (например, `class ISetupDetector` per type). Затрудняет добавление новых сетапов и unit-тесты.
* **D-stsu-2 (LOW).** Конкретные thresholds внутри `detect_*` методов могут быть hard-coded или из `ScalpStrategyConfig`. Требует верификации, что все настройки пробрасываются через config.

## Контракты

### `SetupDetector::detect(ctx, id, now)`

* **Pre.** `ctx.features.is_stale = false`. `ctx.world_state ≠ Disrupted`.
* **Post.** Если условия сетапа выполнены → `optional<Setup>{id, type, entry_zone, stop_zone, target_zone, conviction, generated_at}`. Иначе `nullopt`.
* **Invariant.** Никаких side-effects (детектор stateless).

### `SetupValidator::validate(setup, ctx, now) → SetupValidationResult`

* **Pre.** `setup.generated_at < now`.
* **Post.** `result.is_valid` true/false; при false — `result.cancel_reason` и `result.severity`.

## Производственные риски

* **R-stsu-1.** Слишком жёсткие condition'ы → пропуск всех setup → нет торговли. См. session 4 fix про liquidity_degraded и imbalance_reversed.

## Рекомендации

1. Декомпозиция на per-setup-type детекторы (фабрика).
2. Unit-тесты на real-world fixtures (записанные тики и ожидаемые сетапы).
3. Метрика `strategy_setup_detected_total{type}` и `strategy_setup_canceled_total{type, reason}`.
