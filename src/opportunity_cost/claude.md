# `src/opportunity_cost` — Стоимость упущенной возможности

## Назначение

Per-intent оценка `net edge` (в bps) с учётом execution costs, текущей экспозиции, концентрации, capital exhaustion. Возвращает action: Execute / Defer / Suppress / Upgrade. Drawdown- и consecutive-loss penalty повышают эффективный conviction threshold.

## Границы ответственности

* Композитный score (4 веса: conviction, net_edge, capital_efficiency, urgency).
* Action determination: Execute / Defer / Suppress / Upgrade.
* Threshold scaling по drawdown / consecutive losses.
* Метрики: счётчики действий, last_score, decision_latency.

## Публичные интерфейсы

* `class IOpportunityCostEngine`:
  * `evaluate(intent, exec_alpha, portfolio_ctx, conviction_threshold) → OpportunityCostResult`.
* `class RuleBasedOpportunityCost` — production-impl.
* `OpportunityCostConfig` — все пороги (min_net_expected_bps, execute_min_net_bps, exposure thresholds, weights, drawdown_penalty_scale, …).
* `OpportunityCostResult` — `{action, reason, score, factors}`.
* `enum OpportunityAction {Execute, Defer, Suppress, Upgrade}`.

## Внутренние компоненты

* `opportunity_cost_engine.hpp/cpp`.
* `opportunity_cost_types.hpp` — DTO.

## Зависимости

* `strategy/strategy_types.hpp` (TradeIntent).
* `execution_alpha/execution_alpha_types.hpp` (ExecutionAlphaResult — net edge inputs).
* `clock`, `logging`, `metrics`.

## Потоки данных

```
evaluate(intent, exec_alpha, portfolio_ctx, threshold):
  lock(mutex_)
  effective_threshold = effective_conviction_threshold(threshold, portfolio_ctx)
                        // base + drawdown_penalty_scale·drawdown + consecutive_loss_penalty·losses
  score = compute_score(intent, exec_alpha, portfolio_ctx)
        = w1·conviction + w2·net_edge + w3·capital_efficiency + w4·urgency
  (action, reason) = determine_action(score, portfolio_ctx, intent.conviction, effective_threshold)
  factors = build_factors(...)
  metrics: actions_<action>++, last_score_, last_net_edge_
  return result{action, reason, score, factors}
```

## Race conditions

* `mutex_` — для metrics + state.

## Ошибки проектирования

* **D-oc-1 (LOW).** Веса нормально суммируются в 1.0, но не валидируются runtime — при опечатке в YAML разница нарушает интерпретацию.
* **D-oc-2 (LOW).** `opportunity_cost_bps` из `ExecutionAlpha::Config` — разные источники edge цены; risk inconsistency.

## Контракты

### `evaluate(...) → OpportunityCostResult`

* **Pre.** `intent.conviction ∈ [0, 1]`. `portfolio_ctx.drawdown_pct ≥ 0`. `exec_alpha.net_expected_bps` определено.
* **Post.**
  * `result.score ∈ [0, 1]` после нормализации.
  * `result.action ∈ {Execute, Defer, Suppress, Upgrade}`.
  * При просадке effective_threshold > base_threshold.
* **Invariant.** Action `Suppress` → conviction < effective_threshold.

## Производственные риски

* **R-oc-1.** Mis-tuned weights → перекос в одно измерение, плохие действия.
* **R-oc-2.** drawdown_penalty_scale = 0.05 (correctly tuned per session 3); ранее 0.5 = killer effect.

## Рекомендации

1. Runtime-валидация суммы весов = 1.0.
2. Унифицировать `opportunity_cost_bps` (source of truth — config.opportunity_cost).
3. Метрика `opportunity_cost_action_total{action, reason}`.
4. Test: action-distribution на исторических трейсах.
