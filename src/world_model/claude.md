# `src/world_model` — Минимальный классификатор состояний рынка

## Назначение

Per-symbol классификация рыночного состояния с минимальной surface area. Используется `Uncertainty` (как входной сигнал fragility), `Decision` (state_probabilities — danger penalty в conviction threshold) и `Leverage` (через `adversarial_severity`, который вычисляется inline в pipeline).

> **Scalping refactor (2026-05):** Прежняя 9-state адаптивная машина с гистерезисом, transition-matrix, feedback-EMA и multi-driver scoring (~1998 LOC) удалена. Новый engine — ~211 LOC, без внутреннего state-tracking за пределами `last_[symbol]`. Размер был чрезмерным для одной активной стратегии (`scalp_engine`) на $15 аккаунте.

## Границы ответственности

* Классификация `FeatureSnapshot` → один из 5 фактически используемых `WorldState`: `LiquidityVacuum`, `ToxicMicrostructure`, `StableTrendContinuation`, `ChopNoise`, `ExhaustionSpike`. Остальные значения enum'а никогда не возвращаются.
* Простое отображение state → `WorldStateLabel` (Stable/Transitioning/Disrupted/Unknown).
* `state_probabilities`: 0.7 на primary + 0.075 на каждое из «опасных» состояний (Toxic, Vacuum, Exhaustion, Chop) — для downstream danger-penalty.
* Одна запись `strategy_suitability` для активной стратегии `scalp_engine`: 1.0 при штатном состоянии, 0.3 при `Disrupted`.
* `fragility` вычисляется как простая сумма флагов (spread > 20 bps, book_instability > 0.5, vpin_toxic).
* `last_[symbol]` хранит последний snapshot — используется `current_state()`.

## Что было удалено

* Per-symbol `SymbolContext` (история, transition-matrix 9×9, dwell_ticks).
* Гистерезис, `confirmation_ticks`, `min_dwell_ticks`.
* Эмпирическая persistence + transition matrix.
* `feedback_stats_` (накопление per (state, strategy)).
* Multi-dimension scoring (`signal_suitability`, `execution_suitability`, `risk_suitability`).
* Explanation builder (`top_drivers`, `summary`, conditions audit trail).
* `world_model_history.hpp` (удалён файл).

`SuitabilityConfig::make_default()` возвращает пустую таблицу — оставлен только signature для совместимости с конфиг-лоадером.

## Публичные интерфейсы

* `IWorldModelEngine`:
  * `update(FeatureSnapshot) → WorldModelSnapshot`.
  * `current_state(Symbol) → optional<WorldModelSnapshot>`.
  * `record_feedback(WorldStateFeedback)` — теперь no-op (default-implementation).
  * `performance_stats(state, strategy_id)` — всегда возвращает `nullopt`.
  * `model_version()` — `"3.0.0-min"`.
* `RuleBasedWorldModelEngine` — production-impl.

## Внутренние компоненты

* `world_model_engine.hpp/cpp` — engine (~211 LOC).
* `world_model_types.hpp` — типы (структуры сохранены для downstream compatibility; многие поля заполняются константами по умолчанию).
* `world_model_config.hpp` — пороги. Большая часть полей теперь dormant (engine использует ~7 из ~50). Не удалены ради совместимости с `config_loader`/`config_validator`; чистка — отдельный пас.

## Алгоритм классификации

Приоритет от Disrupted к Trending:

1. `spread_bps >= liquidity_vacuum.spread_bps_critical` → `LiquidityVacuum`.
2. `vpin_toxic` → `ToxicMicrostructure`.
3. `book_instability >= toxic.book_instability_min && spread_bps >= toxic.spread_bps_min` → `ToxicMicrostructure`.
4. `liquidity_ratio < liquidity_vacuum.liquidity_ratio_min` → `LiquidityVacuum`.
5. `adx >= stable_trend.adx_min && |momentum_20| > 0.0005` → `StableTrendContinuation`.
6. `adx <= chop_noise.adx_max` → `ChopNoise`.
7. RSI extreme + reversal momentum → `ExhaustionSpike`.
8. Иначе → `Unknown`.

## Контракты

### `update(snapshot)`

* **Pre:** `snapshot.symbol != ""`.
* **Post:** `last_[snap.symbol]` обновлён. Возвращён `WorldModelSnapshot` со заполненными полями `state`, `label`, `fragility`, `state_probabilities`, `strategy_suitability`, `computed_at`, `symbol`, `model_version`.
* **Invariant:** `state_probabilities.values` суммируются ≤ 1.0; primary state получает 0.7 mass.

## Зависимости

* `features/feature_snapshot.hpp` (вход).
* `clock`, `logging`.

## Concurrency

* Один `std::mutex` под `last_` и весь `update()`. На hot-path вызов из public-WS thread (один per pipeline), contention минимален.

## Текущие риски

* **D-wm-1 (LOW).** `world_model_config.hpp` содержит dormant fields (fragile_breakout, compression, fragility weights, persistence config, hysteresis config, feedback config). Engine их не читает. `config_loader.cpp` всё ещё их парсит. Cleanup отложен до отдельного config-refactor (~200 LOC).
