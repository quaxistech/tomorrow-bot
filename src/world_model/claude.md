# `src/world_model` — Модель состояний мира

## Назначение

Классификация per-symbol макросостояния рынка (9 состояний с гистерезисом, fragility, persistence, transition context). Используется downstream `Uncertainty`, `RiskEngine`, `Decision` для адаптации поведения.

## Границы ответственности

* Классификация на основе текущего `FeatureSnapshot` → `WorldState` (Stable/Disrupted/Transitioning + 6 дополнительных).
* Гистерезис per symbol: `SymbolContext` хранит историю переходов.
* Метрики: `FragilityScore`, `persistence`, `TransitionContext`.
* Feedback: после закрытия сделок — статистика per (state, strategy).
* Версионирование модели для аудита (`model_version()`).

## Публичные интерфейсы

* `class IWorldModelEngine`:
  * `update(FeatureSnapshot) → WorldModelSnapshot`.
  * `current_state(Symbol) → optional<WorldModelSnapshot>`.
  * `record_feedback(WorldStateFeedback)`.
  * `performance_stats(state, strategy) → optional<StatePerformanceStats>`.
  * `model_version() → string`.
* `class RuleBasedWorldModelEngine` — production-impl с config, hysteresis, multi-driver scoring.

## Внутренние компоненты

* `world_model_engine.hpp/cpp` — main engine.
* `world_model_types.hpp` — `WorldState`, `WorldModelSnapshot`, `FragilityScore`, `TransitionContext`, `SymbolContext`, `StateProbabilities`, `StatePerformanceStats`, `WorldModelExplanation`.
* `world_model_history.hpp` — buffer переходов per symbol.
* `world_model_config.hpp` — пороги, веса, hysteresis параметры.

## Зависимости

* `features/feature_snapshot.hpp`.
* `clock`, `logging`.

## Потоки данных

```
update(snapshot):
  lock(mutex_)
  ctx = contexts_[symbol] (или создать)
  immediate = classify_immediate(snap, ctx, explanation)
  state = apply_hysteresis(immediate, ctx, explanation)
  fragility = compute_fragility(snap, state, ctx)
  persistence = compute_persistence(state, ctx)
  transition = compute_transition(state, ctx)
  confidence = compute_confidence(snap, state, ctx, explanation)
  probs = compute_state_probabilities(state, confidence, ctx)
  suitability = compute_suitability(state, snap, ctx)
  drivers = compute_top_drivers(snap, state)
  summary = generate_summary(...)
  return WorldModelSnapshot{state, fragility, persistence, transition, confidence, probs, suitability, drivers, summary}
```

## Race conditions

* `mutex_` под все операции.
* `contexts_`/`feedback_stats_` под mutex.

## Ошибки проектирования

* **D-wm-1 (LOW).** Полный пересчёт всех метрик на каждый `update` — OK для небольшого числа символов, может стать узким местом при росте.
* **D-wm-2 (LOW).** `feedback_stats_` использует ключ `state_index:strategy_id` (string concat) — eventual collision risk при добавлении новых состояний.
* **D-wm-3 (INFO).** `model_version()` возвращает hardcoded "1.0.0" — не отражает изменения в коде.

## Контракты

### `update(FeatureSnapshot snapshot)`

* **Pre.** `snapshot.symbol != ""`. Снапшот валидный (`is_stale = false` рекомендуется).
* **Post.** Возвращён `WorldModelSnapshot` для symbol. `contexts_[symbol]` обновлён.
* **Invariant.** Гистерезис: `state` меняется только если `immediate` подтверждается N тиков подряд. Защита от флаппинга.

### `record_feedback(WorldStateFeedback fb)`

* **Pre.** `fb.state` валидный, `fb.strategy_id != ""`.
* **Post.** `feedback_stats_[key]` обновлён; впоследствии влияет на `compute_suitability`.

## Производственные риски

* **R-wm-1.** Если `feedback_stats_` накапливается без bound — рост памяти.

## Рекомендации

1. Ограничить `feedback_stats_` rolling window (последние N feedbacks per key).
2. `model_version()` — возвращать git-SHA + config-hash.
3. Тест: проверка перехода между всеми 9 состояниями на synthetic features.
