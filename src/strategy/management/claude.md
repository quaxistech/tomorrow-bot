# `src/strategy/management` — Менеджер открытой позиции

## Назначение

Решения hold/reduce/exit для уже открытой позиции, согласно §17-18 ТЗ.

## Границы ответственности

* `evaluate(pos, ctx, now) → PositionManagementResult`.
* Внутри — 3 проверки: structure failure, target reached, quality degradation.

## Публичные интерфейсы

* `class PositionManager`:
  * `evaluate(StrategyPositionContext, StrategyContext, now_ns) → PositionManagementResult`.
* `PositionManagementResult` — `{action, exit_reason, partial_fraction}`.

## Внутренние компоненты

* `position_manager.hpp/cpp`.

## Зависимости

* `strategy/strategy_types.hpp` — `StrategyContext`, `PositionManagementResult`, `ExitReason`.
* `strategy/strategy_config.hpp` — пороги structure/quality/target.
* `features/feature_snapshot.hpp`.

## Потоки данных

`StrategyEngine::handle_position` → `position_manager_.evaluate(pos, ctx, now)` → решение → возможно `build_exit_intent` → `TradeIntent`.

## Race conditions

Stateless.

## Ошибки проектирования

* **D-stmg-1 (LOW).** Параметры порогов — общие на все 4 типа сетапов. Возможно, для разных setup-type'ов оптимальны разные правила exit.

## Контракты

### `evaluate(pos, ctx, now) → PositionManagementResult`

* **Pre.** `pos.has_position = true`. Position-stats валидные.
* **Post.** Возвращён `{action, exit_reason}`. Action ∈ {Hold, Reduce, ExitFull, ExitPartial}.

## Производственные риски

* **R-stmg-1.** `quality_degradation` срабатывает слишком часто → преждевременный exit, потеря edge.

## Рекомендации

1. Per-setup-type конфиги для exit-критериев.
2. Backtest на исторических сделках: распределение причин exit, медианный hold time.
