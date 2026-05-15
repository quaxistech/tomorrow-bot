# `src/regime` — Классификатор режима рынка

## Назначение

Per-symbol классификация в один из 13 detailed regimes (Trending Up/Down, Ranging, Volatile, Choppy, …) с гистерезисом и интеграцией CUSUM для ускорения переключения при структурном слом.

## Границы ответственности

* Классификация на основе ATR/ADX/momentum/volatility.
* Гистерезис: `dwell_ticks`, `candidate_ticks`.
* CUSUM accelerator: при детекции structural break — ускоряет переход.
* Confidence + stability scores.
* Regime hints: рекомендации стратегии (e.g., trend-following vs mean-reversion).

## Публичные интерфейсы

* `class IRegimeEngine`:
  * `classify(FeatureSnapshot) → RegimeSnapshot`.
  * `current_regime(Symbol) → optional<RegimeSnapshot>`.
* `class RuleBasedRegimeEngine` — production-impl с config + hysteresis state per symbol.
* `RegimeSnapshot` — `DetailedRegime`, `confidence`, `stability`, `hints`, `summary`.
* `enum class DetailedRegime` — 13 значений + `Undefined`.

## Внутренние компоненты

* `regime_engine.hpp/cpp` — engine.
* `regime_types.hpp` — `DetailedRegime`, `RegimeSnapshot`, `RegimeStrategyHint`, `ClassificationExplanation`.
* `regime_config.hpp` — пороги (ADX, ATR, momentum), hysteresis ticks, CUSUM weights.

## Зависимости

* `features/feature_snapshot.hpp`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
classify(snapshot):
  lock(mutex_)
  state = hysteresis_[symbol]
  immediate = classify_immediate(snap, explanation)
  cusum = snap.advanced_features.cusum_change_detected (если есть)
  state.confirmed = apply_hysteresis(immediate, confidence, state, explanation, cusum)
  confidence = compute_confidence(snap, state.confirmed, explanation)
  stability = compute_stability(state.confirmed, previous)
  hints = generate_hints(state.confirmed)
  summary = build_summary(...)
  return RegimeSnapshot
```

## Race conditions

* `mutex_` для `snapshots_` и `hysteresis_`.

## Ошибки проектирования

* **D-rg-1 (LOW).** Гистерезис — single tick counter; нет cross-fade между близкими режимами (например, между TrendingUp и StrongTrend). Mitigation: probability mixing.
* **D-rg-2 (LOW).** Hard-coded pороги в config — для разных инструментов оптимальны разные значения. Per-symbol overrides отсутствуют.

## Контракты

### `classify(snapshot) → RegimeSnapshot`

* **Pre.** `snapshot.symbol != ""`. `snapshot.is_stale = false`.
* **Post.** Возвращён `RegimeSnapshot{regime, confidence ∈ [0,1], stability ∈ [0,1], hints, summary}`. `snapshots_[symbol]` обновлён.
* **Invariant.** При `cusum_change_detected = true` минимальный `dwell_ticks` снижается (ускорение переключения).

## Производственные риски

* **R-rg-1.** Misclassification → strategy_allocator выдаст неподходящие веса → стратегия торгует в неверных условиях. Mitigation: low-confidence режимов через `decision/CommitteeDecisionEngine`.

## Рекомендации

1. Per-symbol regime configs (override базовых порогов).
2. Probability mixing вместо hard switching (вернуть `regime_probabilities` map).
3. Regression тест: вход → выход для известных market scenarios (BTC flash crash 2020-03-12, FOMC days, etc.).
