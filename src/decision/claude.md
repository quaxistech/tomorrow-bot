# `src/decision` — Агрегация решений (committee voting)

## Назначение

Агрегирует множество `TradeIntent` от стратегий в единый `DecisionRecord` с conviction, ensemble bonus, time decay, drawdown boost и execution-cost penalty. В системе одна стратегия, но интерфейс готов к multi-strategy committee.

## Границы ответственности

* Voting между intents (по convication-weighted).
* Veto rules: при конфликте Long/Short — побеждает большая сумма conviction либо vето.
* Regime-adaptive thresholds.
* Time decay (exp(-λ·age)).
* Execution cost penalty (через `FeatureSnapshot.spread/depth`).
* Drawdown boost (повышение порога при просадке).
* Ensemble bonus при согласии нескольких стратегий.
* Detection of input time-skew (regime/uncertainty timestamps).

## Публичные интерфейсы

* `class IDecisionAggregationEngine`:
  * `aggregate(symbol, intents, allocation, regime, world, uncertainty, optional<portfolio>, optional<features>) → DecisionRecord`.
* `class CommitteeDecisionEngine` — production-impl.
* `DecisionRecord` — `{action, side, conviction, contributing_strategies, exec_cost, time_skew_ns, summary}`.
* `AdvancedDecisionConfig` — λ time decay, regime-thresholds, ensemble bonus, drawdown boost.

## Внутренние компоненты

* `decision_aggregation_engine.hpp/cpp`.
* `decision_types.hpp`.

## Зависимости

* `strategy/strategy_types.hpp` (TradeIntent).
* `strategy_allocator/allocation_types.hpp`.
* `regime`, `world_model`, `uncertainty`.
* `portfolio` (optional).
* `features` (optional).
* `clock`, `logging`.

## Потоки данных

```
aggregate(symbol, intents, allocation, regime, world, uncertainty, portfolio, features):
  scored_intents = score_each(intents, allocation, regime)
  apply time_decay (per intent)
  apply regime_threshold_factor (boost/dampen)
  apply ensemble_metrics (если ≥2 strategies согласны)
  apply drawdown_boost (если portfolio.current_drawdown_pct > τ)
  apply execution_cost penalty (по features)
  determine winning side (Long/Short/None)
  set conviction, action
  return DecisionRecord
```

## Race conditions

Stateless / stateful по необходимости (зависит от реализации `CommitteeDecisionEngine`); вероятно, stateless.

## Ошибки проектирования

* **D-dec-1 (LOW).** Optional `portfolio`/`features` параметры предоставляют деградированный путь, но pipeline всегда передаёт их → optional-логика для test/backward compat. Можно ужесточить контракт.
* **D-dec-2 (LOW).** Time skew detection: возвращает `int64_t` ns, но не блокирует решение при больших значениях. Должна быть soft invariant + лог.

## Контракты

### `aggregate(...)`

* **Pre.** `intents.size() ≥ 0` (пустой вход → no-action). `allocation.weights` нормализованы.
* **Post.** `DecisionRecord{action, side, conviction ∈ [0,1], summary, contributing_strategies}`.
  * `action = NoAction` если ни один intent не достиг threshold.
  * При конфликте `Long`/`Short` — выбран Side с большей aggregate conviction (или NoAction если разница < dominance threshold).
* **Invariant.** Не возвращает `Hold` действие (это ответственность strategy, а не decision aggregator).

## Производственные риски

* **R-dec-1.** Слишком высокий threshold → нет торговли. Слишком низкий → перетрейдинг.
* **R-dec-2.** Time skew между snapshot'ами (если features ст**arше** regime/uncertainty) ведёт к decision на устаревших данных. Защита через `detect_time_skew`.

## Рекомендации

1. Метрики: `decision_action_total{action}`, `decision_conviction` (gauge).
2. Test: регрессия на синтетических наборах intents с разной conviction.
3. Альтернативный режим — Bayesian model averaging вместо committee voting (для будущего).
