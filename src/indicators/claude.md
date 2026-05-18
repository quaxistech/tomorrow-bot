# `src/indicators` — Технические индикаторы

## Назначение

Каноническая реализация классических технических индикаторов + advanced professional indicators для precision сигналов (run93-94 expansion).

## Границы ответственности

* **Classic stateless** (indicator_engine): SMA, EMA, RSI (Wilder), MACD (Appel), Bollinger (Bollinger), ATR/ADX (Wilder), OBV, VWAP, Rolling VWAP, ROC, Z-Score, Volatility (Hull), Momentum.
* **run93 additions** (indicator_engine): Supertrend (Olson 2008), Stochastic (Lane 1957), EMA pair (9/21) crossover.
* **run94 advanced** (advanced_indicators): Anchored VWAP, CVD + delta divergence, OI tracker (Wyckoff quadrant), Liquidity Sweep, Queue Position, Spoof Detector, Liquidation Heatmap proxy, Funding Bias, Adaptive Thresholds, Bayesian Signal Combiner.

## Входы / выходы

* **Classic**: `std::vector<double>` → `IndicatorResult`, `MacdResult`, `AdxResult`, `BollingerResult`, `VwapResult`, `SupertrendResult`, `StochasticResult`, `EmaPairResult`.
* **Stateful streaming** (run94): `AnchoredVwap`/`CvdTracker`/`OiTracker` — accept on_trade()/on_oi_update() калбэки → snapshot() возвращает текущий state.
* **Pure helpers**: `detect_liquidity_sweep()`, `estimate_queue_position()`, `detect_spoofing()`, `estimate_liquidation_clusters()`, `evaluate_funding_bias()`, `compute_adaptive_thresholds()`, `combine_signals_bayesian()`.

## Публичные интерфейсы

* `class IndicatorEngine` — header содержит ~15 методов (12 classic + Supertrend/Stochastic/EmaPair).
* DTO classic: `IndicatorResult`, `AdxResult`, `BollingerResult`, `MacdResult`, `VwapResult`, `SupertrendResult`, `StochasticResult`, `EmaPairResult` в `indicator_types.hpp`.
* DTO advanced: `AnchoredVwapResult`, `CvdResult`, `OiResult`, `LiquiditySweepResult`, `QueuePositionResult`, `SpoofResult`, `LiquidationProxyResult`, `FundingBiasResult`, `AdaptiveThresholds`, `BayesianSignalScore` в `advanced_indicators.hpp`.

## Внутренние компоненты

* `indicator_engine.hpp/cpp` — classic + Supertrend/Stochastic/EmaPair.
* `indicator_types.hpp` — classic DTO.
* `advanced_indicators.hpp/cpp` — **run94:** Anchored VWAP, CVD, OI, Liquidity Sweep, Queue Position, Spoof, Liquidation, Funding bias, Adaptive thresholds, Bayesian fusion.

## Зависимости

* `common/numeric_utils.hpp`.
* `logging` (для предупреждений).

## Потоки данных

`FeatureEngine::compute_technical(symbol)` берёт текущее окно цен из `CandleBuffer` и вызывает `indicator_engine_->ema/rsi/...` — каждый метод итерирует вектор и возвращает значение на последний bar.

## Race conditions

Stateless — методы const, без mutex. Конкурентный вызов безопасен.

## Ошибки проектирования

* **D-ind-1 (MEDIUM).** Нет инкрементальных версий. Каждый тик пересчитывает full sliding window (O(N) per indicator). Для 12 индикаторов × 1 минута × 500 баров = ~6000 ops/tick — приемлемо, но можно сделать O(1) через streaming (Welford для variance, exponential update для EMA).
* **D-ind-2 (LOW).** `volatility(prices, period)` использует `n-1` Bessel correction; `z_score` использует `n` (population). Документировано, но требует понимания caller'а.
* **D-ind-3 (LOW).** Periods принимаются как `int`; нет валидации `period > 1`. Реализация internally возвращает `valid=false` на bad input, но это runtime behavior.

## Контракты

### `IndicatorEngine::ema(prices, period)`

* **Pre.** `prices.size() ≥ period ∧ period ≥ 1`.
* **Post.** Возвращён `IndicatorResult{value, valid}` где value = EMA на последнем баре, `valid = (size ≥ period)`.

### `IndicatorEngine::rsi(prices, period = 14)`

* **Pre.** `prices.size() ≥ period + 1`.
* **Post.** `value ∈ [0, 100]`, `valid = true`. На bad input — `valid = false ∧ value = 0`.

### Аналогично для остальных.

## Производственные риски

* **R-ind-1.** При недостаточной истории (< period+1 баров) — `valid=false` пропагируется в `FeatureSnapshot`. Стратегия должна явно проверять флаг, иначе принимает решение на нулевых значениях.
* **R-ind-2.** Numerical stability для длинных VWAP windows / больших volume — может потерять точность. Mitigation: Kahan summation.

## Рекомендации

1. Streaming-версии: `EMAState`, `RSIState`, `BollingerState` — O(1) update.
2. Валидация на этапе вызова: `assert(period >= 2)` или `Result<...>`.
3. Бенчмарк под нагрузкой: 1000 calls/s.
