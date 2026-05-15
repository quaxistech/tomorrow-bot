# `src/indicators` — Технические индикаторы

## Назначение

Каноническая реализация классических технических индикаторов. Нет state — функции принимают полный вектор цен и возвращают результат на последний бар.

## Границы ответственности

* SMA, EMA, RSI (Wilder, 1978), MACD (Appel, 1979), Bollinger Bands (Bollinger, 2002), ATR/ADX (Wilder), OBV, VWAP, Rolling VWAP, ROC, Z-Score, Volatility (Hull), Momentum.
* Все параметры по умолчанию следуют каноническим спецификациям (ссылки в комментариях кода).

## Входы / выходы

* Вход: `std::vector<double>` (prices/highs/lows/closes/volumes) + параметры (period).
* Выход: `IndicatorResult{value, valid}`, `MacdResult`, `AdxResult`, `BollingerResult`, `VwapResult`.

## Публичные интерфейсы

* `class IndicatorEngine` — header содержит ~12 методов.
* DTO: `IndicatorResult`, `AdxResult`, `BollingerResult`, `MacdResult`, `VwapResult` в `indicator_types.hpp`.

## Внутренние компоненты

* `indicator_engine.hpp/cpp` — реализация.
* `indicator_types.hpp` — DTO.

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
