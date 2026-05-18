# `src/uncertainty` — Четырёхсигнальный gate для скальпинга

## Назначение

Единственный source-of-truth для масштабирования размера позиции и для severity-входа в conviction threshold. Производит `UncertaintySnapshot` с `level`, `aggregate_score`, `size_multiplier`, `threshold_multiplier`, `recommended_action` и cooldown.

> **Scalping refactor (2026-05):** 9-мерная композитная модель (regime / signal / data / execution / portfolio / ml / correlation / transition / operational) удалена. Текущий engine — ~275 LOC. Сократили до четырёх hard-сигналов, которые реально дают edge на $15-аккаунте, + три soft-сигнала, добавляющих небольшую коррекцию.

## Сигналы

**Hard signals (70% веса, max-of-priorities):**
1. `spread_bps` — насыщается на 25 bps. Узкое место fee-чувствительного аккаунта.
2. `is_feed_fresh = false` → 0.8. Не торгуем на устаревшем фиде.
3. `vpin_toxic` → 0.85; постепенная шкала `vpin` в `[0.4, 0.8]`. Информированный поток.
4. `book_instability` — pre-cascade условия.

**Soft signals (30% веса, среднее):**
5. `regime.confidence` — низкая уверенность режима добавляет шум.
6. `world.fragility` — суммарный fragility-score из world_model.
7. `ml.cascade_probability`, `ml.correlation_break` — ML-side кризис.

Aggregate: `0.7 × max(hard) + 0.3 × avg(soft)`, EMA-сглаживание с конфигурируемой α.

## Уровни и действия

| Level    | Range      | Action          | size_multiplier | threshold_multiplier |
|----------|------------|-----------------|-----------------|----------------------|
| Low      | < 0.25     | Normal          | 1.0             | 1.0                  |
| Moderate | 0.25..0.50 | ReducedSize     | 0.7             | 1.1                  |
| High     | 0.50..0.75 | HigherThreshold | 0.4             | 1.5                  |
| Extreme  | ≥ 0.75     | NoTrade         | size_floor (0.1)| 2.0                  |

Гистерезис: понижение уровня требует доп. запаса `hysteresis_down`. Cooldown: после `consecutive_extreme_for_cooldown` подряд Extreme — блокировка на `cooldown_duration_ns`.

## Публичные интерфейсы

* `IUncertaintyEngine`:
  * v1 `assess(features, regime, world)` — делегирует в v2 с нейтральными pf/ml.
  * v2 `assess(features, regime, world, portfolio, ml_signals)` — primary path.
  * `diagnostics()`, `reset_state()`.
* `RuleBasedUncertaintyEngine` — единственная реализация.

## Что использует выход

* `pipeline/trading_pipeline.cpp` (inline allocator) — **единственный** источник `size_multiplier`. После refactor 2026-05 удалены параллельные множители `regime.hint.weight_multiplier` и `world.strategy_suitability` (double-counting).
* `decision/decision_aggregation_engine.cpp` — `UncertaintyLevel` → severity score для conviction threshold suppression budget (capped 0.25). Раньше `threshold_multiplier` умножался напрямую → unreachable cap. Теперь — additive severity.
* `leverage_engine.cpp` — после refactor нейтрален для Low/Moderate/High (возвращает 1.0). Подавляет leverage только при Extreme (0.6). Раньше суммарно с allocator size_multiplier давал double-suppression.

## Что было удалено

* Размерности: `portfolio_uncertainty` (всё ещё в struct, но всегда 0), большая часть `operational_uncertainty`.
* `compute_drivers` — упрощён до топ-3 hard сигналов.
* Отдельные `compute_*_uncertainty` методы.
* Внутренний state: `peak_score`, `shock_memory`, `consecutive_high`-based defensive logic.

## Контракты

### `assess(features, regime, world, portfolio, ml)`

* **Pre:** `features.symbol != ""`.
* **Post:**
  * `aggregate_score ∈ [0, 1]`.
  * `level ∈ {Low, Moderate, High, Extreme}`.
  * `size_multiplier ∈ [size_floor, 1.0]`.
  * `recommended_action == NoTrade` ⇒ pipeline отклоняет тик.
* **Invariant:** Cooldown forces `NoTrade`/`HaltNewEntries` пока `cooldown_until_ns > now`.

## Зависимости

* `features`, `regime`, `world_model` — входы.
* `portfolio::PortfolioSnapshot`, `ml::MlSignalSnapshot` — forward-decl, чтобы не тянуть тяжёлые заголовки.
* `clock`, `logging`.

## Concurrency

* `mutex_` под `states_`, `snapshots_`, `diagnostics_`.

## Текущие риски

* **D-unc-1 (LOW).** `UncertaintyDimensions` struct сохранена для telemetry, но 4 из 9 полей теперь не несут реальной информации (`portfolio_uncertainty` всегда 0, `signal/transition/operational` повторяют hard-сигналы). Cleanup без отдельной нужды.
* **D-unc-2 (LOW).** `config_loader.cpp` всё ещё парсит `w_regime`, `w_signal`, `w_data_quality`, `w_execution`, `w_portfolio`, `w_ml`, `w_correlation`, `w_transition`, `w_operational` — engine использует только α, threshold_low/moderate/high, hysteresis_down, size_floor, threshold_ceiling, cooldown params. Веса dormant.
