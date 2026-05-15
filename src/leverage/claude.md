# `src/leverage` — Адаптивный движок плеча

## Назначение

Расчёт оптимального плеча для каждой новой позиции на основе 7 множителей: regime, volatility, drawdown, conviction, funding, adversarial, uncertainty. Накладывается Kelly cap (Half-Kelly per Thorp 2006) и EMA сглаживание. Также рассчитывает `liquidation_price` для isolated margin.

## Границы ответственности

* `compute_leverage(LeverageContext) → LeverageDecision`.
* Kelly cap из rolling win_rate / win_loss_ratio.
* EMA smoothing leverage между тиками (anti-flapping).
* `compute_liquidation_price(LiquidationParams)` — bizarre formula для USDT-M isolated.
* `is_liquidation_safe(...)` — проверка буфера до ликвидации.
* `update_edge_stats` — обновление stats из rolling pipeline-stats.
* `update_config` для hot-reload.

## Публичные интерфейсы

* `class LeverageEngine`:
  * Конструктор `(FuturesConfig)`.
  * `compute_leverage(...) → LeverageDecision` (2 overload'а: v1 на 9 параметрах, v2 на `LeverageContext`).
  * `update_edge_stats(win_rate, win_loss_ratio)`.
  * `static double compute_liquidation_price(LiquidationParams)`.
  * `static bool is_liquidation_safe(...)`.
  * `update_config(FuturesConfig)`.
* `LeverageContext` — input DTO.
* `LeverageDecision` — `{leverage, liquidation_price, liquidation_buffer_pct, is_safe, rationale, base/vol/dd/conviction/funding/adversarial/uncertainty factors}`.
* `LiquidationParams` — `{entry_price, position_side, leverage, mmr, taker_fee_rate}`.

## Внутренние компоненты

* `leverage_engine.hpp/cpp`.

## Зависимости

* `config/config_types.hpp` (FuturesConfig).
* `common/types.hpp` (RegimeLabel, UncertaintyLevel, PositionSide).
* `features` (для нормализованного ATR).

## Потоки данных

```
compute_leverage(ctx):
  lock(mutex_)
  base = base_leverage_for_regime(ctx.regime)         // {1, 2, 5, 10, 20} в зависимости
  vol_factor = volatility_multiplier(ctx.atr_normalized)
  dd_factor = drawdown_multiplier(ctx.drawdown_pct)   // sigmoid(tanh)
  conv_factor = conviction_multiplier(ctx.conviction)
  funding_factor = funding_multiplier(ctx.funding_rate, ctx.position_side) // exp penalty
  adv_factor = adversarial_multiplier(ctx.adversarial_severity)
  unc_factor = uncertainty_multiplier(ctx.uncertainty)
  raw_leverage = base · vol_factor · dd_factor · conv_factor · funding_factor · adv_factor · unc_factor
  kelly_cap = kelly_max_leverage()
  raw_leverage = min(raw_leverage, kelly_cap, config_.max_leverage)
  raw_leverage = max(raw_leverage, config_.min_leverage)
  if last_regime != ctx.regime: ema reset
  ema_leverage = α·raw + (1-α)·prev_ema
  final_leverage = round(clamp(ema_leverage, min, max))
  liquidation_price = compute_liquidation_price(...)
  is_safe = is_liquidation_safe(...)
  return LeverageDecision{...}
```

## Race conditions

* `mutex_` для config (hot reload) и edge stats.
* `ema_leverage_` mutable — изменяется внутри const метода `compute_leverage` — потенциальная ловушка для compiler (не-thread-safe). Защищается mutex'ом, но `mutable` в const-методах требует осторожности.

## Ошибки проектирования

* **D-lev-1 (HIGH).** В pipeline параметр `adversarial_severity` всегда подаётся 0.0 (см. Defect-D1 в корне — adversarial_defense не интегрирован). Эффективно `adv_factor = 1.0` всегда → защита не работает.
* **D-lev-2 (MEDIUM).** EMA smoothing с фиксированным α = 0.3 — не настраивается из config. При резких изменениях rolling stats EMA лагает.
* **D-lev-3 (LOW).** `update_edge_stats` принимает `win_rate ∈ [0,1]` без валидации — bad input может привести к нелогичному kelly_cap.
* **D-lev-4 (LOW).** `compute_liquidation_price` использует только `mmr` и `taker_fee_rate` — игнорирует funding accrued. Для долгих hold'ов (что не скальпинг, но защита от ошибок) формула неточна.

## Контракты

### `compute_leverage(LeverageContext ctx)`

* **Pre.** `ctx.entry_price > 0`. `ctx.atr_normalized ≥ 0`. `ctx.conviction ∈ [0,1]`. `ctx.drawdown_pct ≥ 0`.
* **Post.**
  * `result.leverage ∈ [config.min_leverage, config.max_leverage]`.
  * `result.liquidation_price > 0`.
  * `result.is_safe = (buffer_pct ≥ config.min_liquidation_buffer_pct)`.
  * `result.rationale` — человекочитаемое.
* **Invariant.** Все 7 factor'ов ∈ [factor_min, factor_max] (из config).

### `static compute_liquidation_price(params) → double`

* **Pre.** `params.entry_price > 0 ∧ params.leverage ≥ 1`.
* **Post.**
  * Long: `liq = entry × (1 - 1/lev + mmr)`.
  * Short: `liq = entry × (1 + 1/lev - mmr)`.
* **Pure function** (no state).

## Производственные риски

* **R-lev-1.** Adversarial defense не работает (D-lev-1) → переоценка leverage в hostile market.
* **R-lev-2.** Funding penalty экспоненциальный — at extreme funding rates may reduce leverage to 1, что блокирует маржинальные стратегии.
* **R-lev-3.** При hot-reload config возможно «прыжки» leverage между ордерами.

## Рекомендации

1. **R-lev-Big.** Подключить `adversarial_defense` в build, передавать severity в `LeverageContext`.
2. Параметризовать EMA α из config.
3. Валидация input win_rate / win_loss_ratio.
4. Тест: симуляция liquidation price для всех `(entry, leverage, side, mmr)` сценариев.
5. Метрика: `leverage_factor{type}` per pipeline tick.
