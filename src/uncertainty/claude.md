# `src/uncertainty` — Многомерная оценка неопределённости

## Назначение

Композитная оценка неопределённости системы по 9 измерениям (regime, signal, data quality, execution, portfolio, ML, correlation, transition, operational) с гистерезисом, cooldown'ом и рекомендациями по execution mode.

## Границы ответственности

* `IUncertaintyEngine::assess(...)` — два overload'а: v1 (без portfolio/ML) и v2 (полный контекст).
* Per-symbol state: EMA-сглаженный score, peak, consecutive Extreme/High counters, shock memory.
* Cooldown recommendation (стратегиям и pipeline'у).
* Execution mode recommendation (HighUncertainty → Aggressive не желательно).

## Публичные интерфейсы

* `class IUncertaintyEngine`:
  * v1 `assess(features, regime, world)`.
  * v2 `assess(features, regime, world, portfolio, ml_signals)` — main path.
  * `diagnostics() → UncertaintyDiagnostics`.
  * `reset_state()`.
* `class RuleBasedUncertaintyEngine` — production-impl.
* `UncertaintySnapshot` — `level`, `score`, `dimensions`, `drivers`, `cooldown`, `exec_mode`.

## Внутренние компоненты

* `uncertainty_engine.hpp/cpp` — engine.
* `uncertainty_types.hpp` — DTO.

## Зависимости

* `features`, `regime`, `world_model`.
* Forward-declared: `portfolio::PortfolioSnapshot`, `ml::MlSignalSnapshot` (не include — снижает coupling).
* `clock`, `logging`.

## Потоки данных

```
assess(features, regime, world, portfolio, ml):
  lock(mutex_)
  dims = {regime, signal, data_quality, execution, portfolio, ml, correlation, transition, operational}
  raw_score = aggregate(dims, regime)
  state = states_[symbol]
  ema_score = α·raw_score + (1-α)·state.ema_score
  level = apply_hysteresis(ema_score, state)
  update_state(state, raw_score, level, now)
  cooldown = compute_cooldown(state, now)
  exec_mode = determine_execution_mode(level, ema_score, state)
  drivers = compute_drivers(dims)
  return UncertaintySnapshot{level, ema_score, dims, drivers, cooldown, exec_mode}
```

## Race conditions

* `mutex_` под все операции.

## Ошибки проектирования

* **D-unc-1 (MEDIUM).** v1 `assess` делегирует в v2 с нейтральными портфолио/ML. На практике pipeline всегда вызывает v2 (в `trading_pipeline.cpp`), v1 — backward compat. См. session3 fixes — раньше portfolio/ML не передавались.
* **D-unc-2 (LOW).** `transition_uncertainty` ранее вычислялся как `confidence` (инвертированный смысл) — исправлено в session 3 на `0.3 + 0.4·(1-confidence)`. Потенциальная регрессия, нужен тест.
* **D-unc-3 (LOW).** `diagnostics_` — atomic ли? Возвращается копией; thread-safety зависит от реализации.

## Контракты

### `assess(features, regime, world, portfolio, ml) → UncertaintySnapshot`

* **Pre.** Все snapshot'ы валидные. `features.symbol != ""`.
* **Post.**
  * Все 9 dimensions ∈ [0, 1].
  * `score ∈ [0, 1]`.
  * `level ∈ {Low, Moderate, High, Extreme}`.
  * `cooldown.until_ns ≥ now`.
* **Invariant.** Гистерезис: `level` меняется только при пересечении threshold + N тиков (защита от флаппинга).

## Производственные риски

* **R-unc-1.** При завышенной неопределённости стратегия не торгует — потеря opportunity. При заниженной — повышенный risk.

## Рекомендации

1. Калибровка весов dimensions на исторических данных.
2. Метрика `uncertainty_dimension_score{dim}` для observability.
3. Тест: степень корреляции с реализованной волатильностью (за период) — должна быть положительная.
