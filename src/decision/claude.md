# `src/decision` — Intent-gate (de-facto single-strategy)

## Назначение

Принимает один (на данный момент — всегда один) `TradeIntent` от активной стратегии `scalp_engine`, прогоняет через bounded conviction-gate с учётом неопределённости, world-state danger probability, drawdown и execution cost, и либо одобряет, либо возвращает rejection с reason code.

> **Scalping refactor (2026-05):** Полноценный «committee voter» с BUY/SELL conflict resolution, ensemble bonus, regime threshold factor и dominance threshold удалён. Бот эмитит максимум один intent за тик; committee — degenerate. См. `decision_aggregation_engine.cpp:240+` для текущей формулы.

## Что делает

1. **Time skew detection** (если `enable_time_skew_detection`): сравнивает `regime.computed_at`, `uncertainty.computed_at`, `decided_at`. Логирует warn при превышении.
2. **Глобальные вето** (`uncertainty.recommended_action == NoTrade` или `allocation.enabled_count == 0`) → ранний return с `GlobalUncertaintyVeto` / `AllStrategiesDisabled`.
3. **Execution cost estimate** (`enable_execution_cost_modeling`): spread + slippage в bps; если > `max_acceptable_cost_bps` → veto `ExecutionCostTooHigh`.
4. **Per-intent scoring**: aged_conviction = conviction × time_decay, effective_conviction = aged − cost_penalty, weighted_score = effective × allocator_weight.
5. **Bounded conviction threshold** (новая формула — заменяет прежний мультипликативный stack):
   ```
   severity = max(uncertainty_level_severity, danger_prob × 0.7)
   suppression_boost  = min(0.25, severity × 0.30)
   cost_headroom      = min(0.20, total_cost_bps / 200.0)
   threshold = base + suppression_boost + drawdown_threshold_boost + cost_headroom
   threshold = min(threshold, 0.80)
   ```
   * `severity` — max-of-priorities, не сумма (защита от triple-counting overlapping signals).
   * `cost_headroom` — порог растёт со spread+slippage → fee/spread-aware.
   * Hard cap 0.80 (раньше 0.90 — был часто недостижим).
6. **Approval gate:** одобряет если `effective_conviction ≥ threshold`. Иначе rejection: `DrawdownProtection` если без DD-буста прошло бы, иначе `LowConviction`.

## Что было удалено

* `compute_regime_threshold_factor()` — uncertainty engine уже учитывает regime confidence, повторное умножение было double-counting.
* `compute_regime_dominance_threshold()` — dominance был нужен только при BUY/SELL conflict.
* BUY/SELL conflict resolution (`has_buy/has_sell`/`erase_if losing side`) — у нас всегда max 1 intent.
* `compute_ensemble_metrics()` — degenerate при N=1.
* `World state_probabilities × 0.20` additive (был включён в новую severity-формулу).

## Публичные интерфейсы

* `IDecisionAggregationEngine::aggregate(symbol, intents, allocation, regime, world, uncertainty, optional<portfolio>, optional<features>) → DecisionRecord`.
* `CommitteeDecisionEngine` — единственная реализация (имя сохранено для совместимости; «committee» теперь номинальное).
* `AllocationResult` / `StrategyAllocation` — перенесены в `decision/strategy_allocation.hpp` после удаления `src/strategy_allocator/`.

## Контракты

### `aggregate(...)`

* **Pre:** `intents.size() ≥ 0`. Пустой вход → trade_approved = false, NoValidIntents.
* **Post:**
  * `decision.trade_approved ∈ {true, false}`.
  * Если approved → `final_intent.has_value() && final_conviction ∈ [0, 1]`.
  * Если rejected → `rejection_reason != None`.
* **Invariant:** `effective_threshold ≤ 0.80`.

## Зависимости

* `strategy/strategy_types.hpp` (TradeIntent).
* `decision/strategy_allocation.hpp` (типы, ранее в `strategy_allocator/`).
* `regime`, `world_model`, `uncertainty`.
* `portfolio` (optional, для drawdown_boost).
* `features` (optional, для execution_cost).
* `clock`, `logging`.

## Concurrency

Stateless. `aggregate()` thread-safe относительно входов (всё передаётся по const-ref).

## Текущие риски

* **D-dec-1 (LOW).** `EnsembleMetrics` всё ещё в `DecisionRecord` (`aligned_count = 1`, `ensemble_bonus = 0`). Сохранено для совместимости телеметрии — может быть удалено позже.
* **D-dec-2 (LOW).** `record.regime_threshold_factor` всегда 1.0 (поле сохранено для совместимости рационейла).
