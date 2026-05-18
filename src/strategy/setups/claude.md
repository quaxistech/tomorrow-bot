# `src/strategy/setups` — Setup detector & validator

## Назначение

Детекция и валидация скальпинговых сетапов (4 типа) согласно §10, §13-15 ТЗ. Stateless-классы, оперируют `StrategyContext` и возвращают `optional<Setup>` либо `SetupValidationResult`.

## Границы ответственности

* `SetupDetector::detect(ctx, id, now)` — попытка обнаружить любой из 4 типов сетапа.
* `SetupValidator::validate(setup, ctx, now)` — проверка, что сетап остаётся валидным.
* `SetupValidator::can_confirm(setup, ctx, now)` — готов ли к входу.

## run94 Bayesian fusion в detect_momentum (10 indicators)

После прохождения базовых фильтров (mom5/RSI/BB/HTF trend), сетап получает **Bayesian likelihood ratio fusion** из 10 indicators:

1. **Supertrend trend** (LR 2.0/0.5 → 4× favor) — primary trend filter
2. **EMA pair (9/21)** (LR 1.7/0.6) — micro-trend confirmation
3. **Stochastic** (LR 1.5/0.7 для oversold/overbought, 1.8/0.6 для crossovers)
4. **Anchored VWAP** — bias above/below + 2σ band reversion (1.4/0.7 bias, 0.5/1.8 extreme)
5. **CVD normalized** + divergence (1.6/0.6 momentum, 2.0/0.5 divergence)
6. **OI 4-quadrant** Wyckoff (1.5/0.7 healthy trend, 0.9/1.2 weak coverage)
7. **Liquidity sweep** (0.4/2.0 — fade wick direction)
8. **Funding bias** mean revert (1.4/0.7 при crowding>0.5)
9. **Spoof intensity** > 0.7 → 0.85/0.85 (degrade both directions)
10. **Liq cascade risk** > 0.7 → adjust against dominant side at risk

Posterior `p_bullish/p_bearish` через Naive Bayes log-odds. Setup проходит только при **p > 0.55** в направлении strategy signal.

Confidence = base + imb_strength × spread_factor × 0.2 + **Bayesian confidence × 0.25**.

## TP/SL расчёт (run95 calibration)

```
SL = mid ∓ atr × atr_stop_mult_<setup>
TP = mid ± atr × atr_target_mult_<setup>
```

Default multipliers (run95): momentum/pullback = 1.0/1.8, retest/rejection = 1.2/2.2. R:R ≈ 1.8. Wider SL чем в run93 (0.8) для защиты от slippage 46 bps avg observed на market close в thin alts.

## Публичные интерфейсы

* `class SetupDetector` — методы:
  * `detect(ctx, id, now) → optional<Setup>`.
  * Внутренние: `detect_momentum`, `detect_retest`, `detect_pullback`, `detect_rejection`.
* `class SetupValidator`:
  * `validate(setup, ctx, now) → SetupValidationResult`.
  * `can_confirm(setup, ctx, now) → bool`.

## Внутренние компоненты

* `setup_lifecycle.hpp/cpp` — обе реализации.

## Зависимости

* `strategy/strategy_types.hpp` — `Setup`, `SetupValidationResult`, `StrategyContext`.
* `strategy/strategy_config.hpp` — `ScalpStrategyConfig`.
* `features/feature_snapshot.hpp`.

## Потоки данных

`StrategyEngine::handle_pre_entry` → `setup_detector_.detect(ctx, ...)` → если есть `Setup`, переход state machine → периодический `setup_validator_.validate(setup, ctx, ...)` пока не `can_confirm` или `validation.canceled`.

## Race conditions

Stateless. Конкурентный вызов безопасен.

## Ошибки проектирования

* **D-stsu-1 (MEDIUM).** Логика детекции каждого типа сетапа — самостоятельный набор условий по `FeatureSnapshot`; нет общей абстракции (например, `class ISetupDetector` per type). Затрудняет добавление новых сетапов и unit-тесты.
* **D-stsu-2 (LOW).** Конкретные thresholds внутри `detect_*` методов могут быть hard-coded или из `ScalpStrategyConfig`. Требует верификации, что все настройки пробрасываются через config.

## Контракты

### `SetupDetector::detect(ctx, id, now)`

* **Pre.** `ctx.features.is_stale = false`. `ctx.world_state ≠ Disrupted`.
* **Post.** Если условия сетапа выполнены → `optional<Setup>{id, type, entry_zone, stop_zone, target_zone, conviction, generated_at}`. Иначе `nullopt`.
* **Invariant.** Никаких side-effects (детектор stateless).

### `SetupValidator::validate(setup, ctx, now) → SetupValidationResult`

* **Pre.** `setup.generated_at < now`.
* **Post.** `result.is_valid` true/false; при false — `result.cancel_reason` и `result.severity`.

## Производственные риски

* **R-stsu-1.** Слишком жёсткие condition'ы → пропуск всех setup → нет торговли. См. session 4 fix про liquidity_degraded и imbalance_reversed.

## Рекомендации

1. Декомпозиция на per-setup-type детекторы (фабрика).
2. Unit-тесты на real-world fixtures (записанные тики и ожидаемые сетапы).
3. Метрика `strategy_setup_detected_total{type}` и `strategy_setup_canceled_total{type, reason}`.
