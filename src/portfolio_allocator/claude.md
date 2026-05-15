# `src/portfolio_allocator` — Иерархический сайзинг

## Назначение

Расчёт размера новой позиции с учётом нескольких уровней ограничений: концентрация, бюджет стратегии, volatility targeting, Kelly fraction, drawdown scaling, ликвидность стакана и biexchange filters (min_qty/min_notional/precision).

## Границы ответственности

* `compute_size_v2(intent, portfolio, AllocationContext, uncertainty_size_multiplier) → SizingResult`.
* Volatility targeting: 15% годовой target.
* Kelly fraction (default 0.25 = quarter-Kelly).
* Drawdown scaling: linear от 5% до 15% drawdown_pct → размер от 100% до min_size_fraction.
* ADV / book participation caps.
* Apply exchange filters: round to step, обрезать по min/max.
* `set_market_context` — обновление realized_vol/regime/win_rate/win_loss_ratio.

## Публичные интерфейсы

* `class IPortfolioAllocator`:
  * `compute_size(intent, portfolio, multiplier) → SizingResult`.
  * `compute_size_v2(intent, portfolio, AllocationContext, multiplier) → SizingResult` — preferred.
  * `set_market_context(vol, regime, win_rate, ratio)`.
  * `update_global_budget(double)`.
* `class HierarchicalAllocator` — production-impl с `Config{max_concentration_pct, ..., kelly_fraction, drawdown_scale_*}`.
* `SizingResult` — `{suggested_quantity, max_quantity, multipliers, applied_caps}`.
* `AllocationContext` — input DTO с `current_price`, `book_depth`, `adv`, `existing_strategy_exposure` и т.п.

## Внутренние компоненты

* `portfolio_allocator.hpp/cpp`.
* `allocation_types.hpp` — DTO + `BudgetHierarchy`, `ExchangeFilters`.

## Зависимости

* `strategy/strategy_types.hpp` (TradeIntent).
* `portfolio/portfolio_types.hpp`.
* `regime/regime_types.hpp`.
* `logging`.

## Потоки данных

```
compute_size_v2:
  base_size = intent.suggested_quantity * uncertainty_multiplier
  vol_mult = compute_volatility_multiplier(ctx)  // Kelly + vol target
  regime_mult = compute_regime_multiplier(regime)
  drawdown_scale = compute_drawdown_scale(portfolio.drawdown_pct)
  liquidity_cap = compute_liquidity_cap(price, ctx)
  budget_limit = compute_budget_limit(portfolio)
  concentration_limit = compute_concentration_limit(portfolio)
  strategy_limit = compute_strategy_limit(intent, portfolio)
  notional = base_size · price · vol_mult · regime_mult · drawdown_scale
  notional = min(notional, liquidity_cap, budget_limit, concentration_limit, strategy_limit)
  qty = notional / price
  apply_exchange_filters(qty, price, filters, max_affordable, result)
  return SizingResult{suggested_quantity, ...}
```

## Race conditions

* `context_mutex_` для realized_vol/regime/win_rate/win_loss_ratio.

## Ошибки проектирования

* **D-pa-1 (MEDIUM).** Kelly fraction default 0.25 — четверть Kelly. Исторически использовался hardcoded 0.5; в session 3 переход на adaptive Kelly через rolling stats.
* **D-pa-2 (LOW).** `compute_size` (v1) и `compute_size_v2` дублируют логику — backwards-compat. Pipeline вызывает только v2.
* **D-pa-3 (LOW).** Множители каскадно перемножаются; при экстремальных значениях (drawdown 15% × low conviction × extreme uncertainty) размер может стать subatomic, что не ловится exchange-filter (min_qty) → SizingResult.suggested_quantity = 0 → silent skip.

## Контракты

### `compute_size_v2(intent, portfolio, ctx, uncertainty_mult) → SizingResult`

* **Pre.** `intent.signal_intent ∈ {LongEntry, ShortEntry}`. `portfolio.computed_at` свежий. `ctx.current_price > 0`.
* **Post.**
  * `result.suggested_quantity ≥ 0`.
  * `result.max_quantity ≥ result.suggested_quantity`.
  * Все limits применены, информация в `applied_caps`.
* **Invariant.** Если все caps соблюдены и базовый размер > 0 — `suggested_quantity > 0`. Если `suggested_quantity = 0` — должен быть указан `applied_cap` (например, `MinNotional`).

## Производственные риски

* **R-pa-1.** Неоптимальный Kelly (без адаптации) → over-sizing → ускоренная просадка.
* **R-pa-2.** Liquidity cap основан на `book_depth` snapshot; при глубокой книге, но низкой реальной ликвидности — slippage.

## Рекомендации

1. Удалить v1 `compute_size`; оставить только v2.
2. Логировать `applied_caps` при `suggested_quantity = 0` — для diag.
3. Тест: invariant для extreme inputs (drawdown 50%, conviction 0.99).
4. Метрика `allocator_cap_applied_total{cap}`.
