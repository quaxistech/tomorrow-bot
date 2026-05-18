# `src/features` — Признаки рынка

## Назначение

`FeatureEngine` — собирает технические + микроструктурные + execution-context фичи в единый `FeatureSnapshot`. `AdvancedFeatureEngine` — продвинутые фичи (CUSUM, VPIN, Volume Profile, Time-of-Day).

## Границы ответственности

* Хранение `CandleBuffer<500>` и `TradeBuffer<1000>` per symbol.
* Last ticker per symbol.
* Расчёт `TechnicalFeatures` (через `IndicatorEngine`).
* Расчёт `MicrostructureFeatures` (book depth, spread, imbalance).
* Расчёт `ExecutionContextFeatures` (queue, churn, fill probability inputs).
* Проверка готовности (`is_ready`) — достаточно ли истории.

## Входы / выходы

* Вход: `on_candle/on_trade/on_ticker` — обновление buffers/state.
* Выход: `compute_snapshot(symbol, book) → optional<FeatureSnapshot>`.

## Публичные интерфейсы

* `class FeatureEngine`:
  * Конструктор `(Config, IndicatorEngine, IClock, ILogger, IMetricsRegistry)`.
  * `on_candle/on_trade/on_ticker(...)` — обновление buffer'ов и stateful trackers.
  * **run94 external updates**: `update_open_interest(symbol, oi_usdt, ts_ns)`, `update_funding_rate(symbol, rate_8h)` — feed данных из REST polls.
  * `compute_snapshot(symbol, book) → optional<FeatureSnapshot>`.
  * `is_ready(symbol) → bool`.
* `class AdvancedFeatureEngine` — расширения (CUSUM, VPIN, VolumeProfile, TimeOfDay).
* `FeatureSnapshot` — DTO со всеми фичами.
* `FeatureEngine::Config` — periods для всех индикаторов, `feed_freshness_ns`, `primary_interval`.

### run93-94 feature additions в TechnicalFeatures

* **run93** (in compute_technical from `indicator_engine`):
  - `supertrend_value/trend/flipped` — ATR trend follower (10, 3.0)
  - `stoch_k/d`, `stoch_overbought/oversold/bull_cross/bear_cross` — Stochastic (5, 3, 3)
  - `ema_fast_9/slow_21`, `ema_pair_trend`, `ema_pair_bull_cross/bear_cross`, `ema_pair_separation_bps`
* **run94 stateful trackers** (per-symbol streaming on_trade/update_*):
  - **Anchored VWAP** (session anchor, reset на новый UTC day): `avwap`, `avwap_upper/lower_1sigma`, `avwap_upper/lower_2sigma`, `avwap_price_vs_vwap_bps`
  - **CVD tracker** (taker buy vs sell pressure): `cvd`, `cvd_change_recent`, `cvd_normalized`, `cvd_bullish/bearish_divergence`
  - **OI tracker** (4-quadrant Wyckoff): `oi_current`, `oi_change_recent_pct`, `oi_trend_quadrant`
* **run94 pure-function indicators** (in compute_technical / compute_snapshot):
  - **Liquidity Sweep**: `liq_sweep_high/low`, `liq_sweep_recovery_pct`
  - **Funding bias**: `funding_rate_8h`, `funding_crowding_side/intensity`, `funding_recommended_bias`
  - **Liquidation proxy**: `liq_upside/downside_cluster_pct`, `liq_cascade_risk_score`, `liq_dominant_side`
  - **Spoof detection** (compute_snapshot, uses top-of-book + cancel burst): `spoof_bid/ask`, `spoof_intensity`

## Внутренние компоненты

* `feature_engine.hpp/cpp` — main.
* `feature_snapshot.hpp` — DTO.
* `microstructure_features.hpp` — отдельные подфункции (spread, imbalance, depth ratios).
* `advanced_features.hpp` — extended features.

## Зависимости

* `indicators` — для tech indicators.
* `buffers` — для удержания истории.
* `order_book` — для current book.
* `normalizer` — для NormalizedX events.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
on_candle/trade/ticker (под mutex_)
  → push в соответствующий буфер
  → обновить last_ticker (для тикера)
compute_snapshot(symbol, book)
  → lock(mutex_)
  → проверка готовности (буферы достаточно полны)
  → compute_technical(symbol) — full recompute через IndicatorEngine
  → compute_microstructure(ticker, book) — top_of_book, depth, imbalance
  → compute_execution_context(ticker, book)
  → собрать FeatureSnapshot{...}
  → return optional<FeatureSnapshot>
```

## Race conditions

* `mutex_` под все операции (push + compute).
* Mutex может стать contention point при >100 events/sec на 5+ символов. Каждый push блокирует потенциальный compute, и наоборот.

## Ошибки проектирования

* **D-feat-1 (HIGH).** Полный пересчёт индикаторов на каждый `compute_snapshot` (O(N×K) где N=window size, K=12 индикаторов). На активном инструменте — десятки ms/tick. См. § 7 Bottleneck-3 в корне.
* **D-feat-2 (MEDIUM).** `compute_snapshot` под единым mutex'ом — блокирует все другие пушы во время compute. Mitigation: COW/seqlock pattern или per-symbol mutex.
* **D-feat-3 (MEDIUM).** Buffer size 500/1000 хардкоден через template; не настраивается из YAML.
* **D-feat-4 (LOW).** `last_trade_received_ns_` используется для freshness-проверки, но в момент `compute_snapshot` уже может быть очень старым. Контракт `feed_freshness_ns` не зашит во flag в `FeatureSnapshot` (требует верификации — поле `is_stale` в `FeatureSnapshot`).

## Контракты

### `FeatureEngine::compute_snapshot(symbol, book)`

* **Pre.** `book.symbol = symbol`. `is_ready(symbol) = true`.
* **Post.**
  * Успех: возвращён `FeatureSnapshot` со всеми полями. `snapshot.computed_at = clock_->now()`. `snapshot.is_stale = (now - last_event_ts > config_.feed_freshness_ns)`.
  * Неудача (`is_ready = false`): `nullopt`.
* **Invariant.** Snapshot — pure read, не меняет состояние engine.

### `FeatureEngine::on_*(event)`

* **Pre.** `event.symbol != ""`.
* **Post.** Соответствующий buffer обновлён, последняя метка времени для symbol — текущая.

## Производственные риски

* **R-feat-1.** Long mutex_ hold во время `compute_snapshot` блокирует WS push. Может вызвать backlog.
* **R-feat-2.** Если `is_ready` false → нет snapshot → pipeline не торгует, но и не получает диагностического сигнала, кроме гейтового счётчика.

## Рекомендации

1. **R-feat-Big.** Streaming-версии индикаторов (см. recommendations в `indicators/`).
2. Per-symbol mutex (не глобальный).
3. Конфигурируемые размеры буферов.
4. Метрика `feature_engine_compute_latency_ns` (histogram).
5. Diagnostic snapshot: `FeatureEngine::diag()` возвращает буфер sizes, last event ts, готовность по символам.
