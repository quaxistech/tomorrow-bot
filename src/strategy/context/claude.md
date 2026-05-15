# `src/strategy/context` — Оценка качества рыночного контекста

## Назначение

Per-tick оценка пригодности рыночного контекста для входа в сделку. Возвращает `MarketContextQuality ∈ {Excellent, Good, Marginal, Poor, Invalid}`.

## Границы ответственности

* Композитная оценка по features + regime + uncertainty + HTF.
* Не принимает торговое решение — только классифицирует контекст.
* Используется `SetupDetector`/`SetupValidator` для отбраковки сетапов до полного вычисления.

## Публичные интерфейсы

* `class MarketContextEvaluator`:
  * `evaluate(StrategyContext) → MarketContextQuality`.

## Внутренние компоненты

* `market_context.hpp/cpp`.

## Зависимости

* `strategy/strategy_types.hpp`.
* `features`, `regime`, `uncertainty`.

## Потоки данных

`StrategyEngine::evaluate` → `context_eval_.evaluate(ctx)` → quality. Если quality `Poor`/`Invalid` — пропуск тика.

## Race conditions

Stateless.

## Ошибки проектирования

* **D-stcx-1 (LOW).** Веса/пороги heuristic — нет калибровки на back-test. Параметры из `ScalpStrategyConfig`.

## Контракты

### `evaluate(ctx) → MarketContextQuality`

* **Pre.** `ctx.features.is_stale = false`.
* **Post.** Возвращена детерминированная классификация по input'у.

## Производственные риски

* **R-stcx-1.** Слишком строгий filter → пропуск всех setup'ов.

## Рекомендации

1. Метрика `strategy_context_quality{quality}`.
2. Distribution тесты на исторических данных (не должно быть >70% Poor в нормальных условиях).
