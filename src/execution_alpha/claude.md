# `src/execution_alpha` — Execution Alpha (maker/taker/PostOnly)

## Назначение

Per-intent выбор стиля исполнения (Passive/Aggressive/Hybrid/PostOnly/NoExecution), расчёт urgency, оценка вероятности заполнения, оценка net edge, генерация SlicePlan для крупных ордеров (через TWAP).

## Границы ответственности

* Style determination: Passive / Aggressive / Hybrid / PostOnly / NoExecution.
* Urgency: f(CUSUM, time-of-day, momentum).
* Adverse selection: weighted aggregation of VPIN + book imbalance + queue depletion + churn.
* Fill probability: estimate based on book + style.
* Net expected return (bps) = expected_alpha − costs (spread + fees + slippage).
* Limit price computation (weighted_mid + offset).
* Slice planning (для размеров > L5 depth threshold).
* Pair execution alpha (joint EIS для long+short).

## Публичные интерфейсы

* `class IExecutionAlphaEngine`:
  * `evaluate(intent, features, uncertainty) → ExecutionAlphaResult`.
  * `evaluate_pair(long_intent, short_intent, features, uncertainty) → PairExecutionAlphaResult`.
* `class RuleBasedExecutionAlpha` — production-impl с `Config{>20 параметров}`.
* `ExecutionAlphaResult` — `{style, limit_price, slice_plan, fill_probability, net_expected_bps, decision_factors}`.
* `enum ExecutionStyle {Passive, Aggressive, Hybrid, PostOnly, NoExecution}`.

## Внутренние компоненты

* `execution_alpha_engine.hpp/cpp`.
* `execution_alpha_types.hpp` — DTO.

## Зависимости

* `strategy/strategy_types.hpp`, `features`, `uncertainty`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
evaluate(intent, features, uncertainty):
  validate_features (graceful degradation если плохое качество)
  urgency = compute_urgency(intent, features, factors)
  adverse_score = estimate_adverse_selection(features, factors)
  imbalance = get_directional_imbalance(intent.side, features)
  style = determine_style(intent, features, urgency, adverse_score, imbalance)
  fill_prob = estimate_fill_probability(style, intent, features, imbalance)
  quality = estimate_quality(...)
  limit_price = compute_limit_price(style, intent, features)
  slice_plan = compute_slice_plan(intent, features) (если notional > threshold)
  net_expected_bps = expected_alpha − exec_cost − slippage_estimate
  return ExecutionAlphaResult
```

## Race conditions

`RuleBasedExecutionAlpha` stateless (или подобно — нужно verify). Конкурентный вызов безопасен.

## Ошибки проектирования

* **D-ea-1 (MEDIUM).** Config содержит много магических чисел (20+ параметров), mostly из научных статей. Каждый параметр требует отдельной калибровки.
* **D-ea-2 (LOW).** `validate_features` — graceful degradation; при плохих данных возвращает Conservative defaults вместо `NoExecution`. Может скрывать реальные проблемы.
* **D-ea-3 (LOW).** `evaluate_pair` использует joint EIS = EIS(long) + EIS(short) + correlation_penalty — но correlation для пары на одном базовом активе (long+short) очевиден; формула может быть упрощена.

## Контракты

### `evaluate(intent, features, uncertainty) → ExecutionAlphaResult`

* **Pre.** `intent.side ∈ {Buy, Sell}`. `features.is_stale = false` (рекомендуется).
* **Post.**
  * `result.style != Invalid`.
  * При `style = NoExecution` — pipeline блокирует ордер.
  * При `style = PostOnly` → fill_probability ≤ конфигурированный лимит.
* **Invariant.** Style и limit_price согласованы (Passive → limit с небольшим offset; Aggressive → cross spread).

## Производственные риски

* **R-ea-1.** Слишком pessimistic adverse_score → все ордера в `NoExecution` → нет торговли.
* **R-ea-2.** Слишком optimistic fill_probability → ожидание заполнения, которое не происходит → opportunity cost (но `OpportunityCost` engine уже учитывает).

## Рекомендации

1. Калибровка config на исторических данных (сравнение predicted vs realized fill_probability).
2. Метрики: `exec_alpha_style_total{style}`, `exec_alpha_fill_probability` (histogram), `exec_alpha_net_bps` (histogram).
3. Регулярный sanity check: avg `net_expected_bps` должен быть > taker fees.
