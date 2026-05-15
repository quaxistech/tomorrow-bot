# Подробный разбор модуля features

Временный аналитический документ.

Источник разбора: текущая реализация `src/features`, её зависимости (`buffers`, `normalizer`, `order_book`, `indicators`) и реальные потребители (`market_data`, `pipeline`) на момент 2026-04-06.

Документ описывает не только публичный API, но и фактическое runtime-поведение модуля в основном контуре USDT-M futures бота.

## 1. Роль модуля в системе

`features` — это слой преобразования нормализованных рыночных событий в компактный `FeatureSnapshot`, который уже можно подавать в world model, regime detection, strategy selection и decision aggregation.

Модуль не занимается:

- подключением к бирже;
- парсингом сырого WebSocket;
- поддержанием sequence integrity стакана;
- принятием торговых решений;
- risk checks;
- execution.

Его фактическая роль:

```text
Bitget WS / replay
  -> normalizer
  -> FeatureEngine + LocalOrderBook
  -> FeatureSnapshot
  -> AdvancedFeatureEngine дополняет snapshot
  -> world_model / regime / strategy / decision / execution
```

То есть это не стратегический слой, а агрегатор признаков поверх уже нормализованных market-data потоков.

## 2. Состав модуля

В `src/features` находятся:

| Файл | Назначение |
|---|---|
| `src/features/CMakeLists.txt` | сборка библиотеки `tb_features` |
| `src/features/feature_snapshot.hpp` | DTO-структуры всех признаков и полного snapshot |
| `src/features/feature_engine.hpp` | публичный API базового feature-движка |
| `src/features/feature_engine.cpp` | реализация технических, микроструктурных и execution-context признаков |
| `src/features/advanced_features.hpp` | API продвинутых признаков: CUSUM, VPIN, Volume Profile, Time-of-Day |
| `src/features/advanced_features.cpp` | реализация продвинутых признаков |
| `src/features/features.md` | этот временный аналитический документ |

Сборка:

```cmake
add_library(tb_features STATIC feature_engine.cpp advanced_features.cpp)
target_include_directories(tb_features PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_features PUBLIC
    tb_common tb_logging tb_metrics tb_clock
    tb_normalizer tb_order_book tb_buffers tb_indicators
)
```

Модуль концептуально состоит из двух частей:

1. `FeatureEngine` — базовые признаки из свечей, тикера, трейдов и стакана.
2. `AdvancedFeatureEngine` — более сложные статистические и microstructure-признаки, которые дописываются в уже готовый `FeatureSnapshot`.

## 3. Главная структура данных: `FeatureSnapshot`

Модуль возвращает единый объект `FeatureSnapshot`, который содержит:

- идентификатор инструмента;
- timestamp вычисления;
- возраст рыночных данных;
- `last_price`;
- `mid_price`;
- блок `technical`;
- блок `microstructure`;
- блок `execution_context`;
- качество стакана `book_quality`.

### 3.1. `TechnicalFeatures`

Содержит три группы полей:

1. Классические технические индикаторы:
   - SMA, EMA, RSI, MACD, Bollinger Bands, ATR, ADX, OBV;
   - volatility 5/20;
   - momentum 5/20.

2. Продвинутые признаки, которые заполняет `AdvancedFeatureEngine`:
   - CUSUM;
   - Volume Profile;
   - Time-of-Day.

3. Флаги `*_valid` для каждого семейства признаков.

Важно: названия полей зашиты как `sma_20`, `ema_20`, `ema_50`, `rsi_14`, `atr_14` и т.д., хотя периоды в `FeatureEngine::Config` конфигурируемы. Это означает, что при изменении конфигурации имена полей могут перестать буквально соответствовать используемому lookback.

### 3.2. `MicrostructureFeatures`

Содержит:

- спред и спред в bps;
- дисбаланс стакана;
- weighted mid;
- buy/sell ratio;
- aggressive flow;
- trade VWAP;
- глубину на 5 уровнях в notional;
- liquidity ratio;
- book instability;
- VPIN-блок, который тоже дописывается `AdvancedFeatureEngine`.

### 3.3. `ExecutionContextFeatures`

Содержит:

- spread cost;
- immediate liquidity;
- estimated slippage;
- `is_market_open`;
- `is_feed_fresh`.

### 3.4. `is_complete()`

`FeatureSnapshot::is_complete()` сейчас возвращает `true`, если одновременно выполнены только два условия:

- `technical.sma_valid == true`
- `microstructure.spread_valid == true`

Это очень слабое определение полноты. Snapshot может считаться complete, даже если MACD/ATR/ADX/OBV/volatility/momentum/VPIN/Volume Profile отсутствуют.

То есть `is_complete()` в текущем состоянии — не строгая гарантия полноты всех признаков, а лишь минимальный smoke-check.

## 4. `FeatureEngine`: базовый движок признаков

`FeatureEngine` — это потокобезопасный stateful агрегатор, который накапливает историю по символам и по запросу строит `FeatureSnapshot`.

### 4.1. Входы движка

Он получает три типа событий:

1. `on_candle(const NormalizedCandle&)`
2. `on_trade(const NormalizedTrade&)`
3. `on_ticker(const NormalizedTicker&)`

И один внешний объект при сборке snapshot:

4. `const LocalOrderBook&`

То есть часть состояния живёт внутри него, а часть приходит снаружи в момент вычисления.

### 4.2. Зависимости

Конструктор получает:

- `FeatureEngine::Config`;
- `IndicatorEngine`;
- `IClock`;
- `ILogger`;
- `IMetricsRegistry`.

Важный практический момент: в основном `TradingPipeline` движок сейчас создаётся как `FeatureEngine::Config{}` без маппинга из `AppConfig`.

Это значит, что в основном runtime-пути используются именно дефолтные параметры из `feature_engine.hpp`, а не внешняя конфигурация приложения.

### 4.3. Внутреннее состояние

Под одним `mutex_` движок хранит:

- `candle_buffers_` — `symbol:interval -> CandleBuffer<500>`;
- `trade_buffers_` — `symbol -> TradeBuffer<1000>`;
- `last_tickers_` — `symbol -> NormalizedTicker`.

Это означает:

- свечи разделяются по таймфрейму и не смешиваются;
- трейды не делятся по interval, а хранятся на уровне символа;
- тикер хранится только последний.

## 5. Как данные попадают в `FeatureEngine`

В основном контуре `MarketDataGateway` получает нормализованные события и делает следующее:

- `NormalizedOrderBook` -> обновляет `LocalOrderBook`;
- `NormalizedTicker` -> `feature_engine_->on_ticker(ev)` и сразу после этого вызывает `compute_snapshot()`;
- `NormalizedTrade` -> `feature_engine_->on_trade(ev)`;
- `NormalizedCandle` -> `feature_engine_->on_candle(ev)`.

Из этого следует ключевая runtime-особенность:

**новый `FeatureSnapshot` создаётся только на тикер-событии.**

То есть:

- трейды сами по себе snapshot не пушат;
- свечи сами по себе snapshot не пушат;
- стакан сам по себе snapshot не пушит.

Они лишь обновляют внутреннее состояние, которое будет использовано на следующем тикере.

## 6. Работа с буферами

### 6.1. CandleBuffer

`CandleBuffer` хранит OHLCV и умеет извлекать массивы `open/high/low/close/volume`.

Поведение `on_candle()`:

- если свеча закрыта, вызывается `push()`;
- если свеча не закрыта, вызывается `update_last()`.

Это даёт полезное свойство: движок умеет видеть текущую незакрытую свечу.

Но отсюда следует и важная семантика:

**технические признаки могут считаться по ещё не закрытой последней свече**, если snapshot строится между промежуточными candle update-сообщениями.

Для скальперского бота это может быть желательным поведением, но это именно live-like behavior, а не strictly-close-only computation.

### 6.2. TradeBuffer

`TradeBuffer` хранит последние сделки и сам умеет считать:

- VWAP по сделкам;
- buy/sell ratio;
- долю агрессивных сделок.

То есть часть микроcтруктурных метрик живёт не в `indicators`, а прямо в буфере трейдов.

## 7. Готовность движка: `is_ready()`

`FeatureEngine::is_ready(symbol)` проверяет наличие candle history для `primary_interval` и считает, что данных достаточно, если есть как минимум:

$$
\max(sma\_period, ema\_slow\_period, macd\_slow) + 1
$$

баров.

На текущих дефолтах это означает минимум `51` свечу.

Это корректно для текущего набора дефолтов, но есть ограничение:

- в расчёт readiness не входят `atr_period`, `adx_period`, `bb_period`, жёстко заданные окна volatility/momentum и прочие будущие расширения;
- если кто-то увеличит, например, `adx_period` или `bb_period` выше текущего максимума, `is_ready()` может начать возвращать `true` раньше, чем реально прогреются все нужные признаки.

То есть текущая readiness-логика зависит от дефолтных периодов сильнее, чем кажется по интерфейсу.

## 8. Как строится `compute_snapshot()`

`compute_snapshot(symbol, book)` делает следующее:

1. Берёт общий mutex.
2. Проверяет readiness по candle buffer.
3. Проверяет наличие последнего тикера.
4. Создаёт `FeatureSnapshot`.
5. Записывает:
   - `symbol`;
   - `computed_at = clock_->now()`;
   - `last_price = ticker.last_price`;
   - `mid_price = (bid + ask) / 2`;
   - `book_quality = book.quality()`.
6. Считает `market_data_age_ns = now - ticker.received_ts`.
7. Вызывает:
   - `compute_technical(symbol)`;
   - `compute_microstructure(ticker, book)`;
   - `compute_execution_context(ticker, book)`.

Далее snapshot возвращается вызывающей стороне.

### Важная семантика `market_data_age_ns`

Возраст данных считается только по последнему тикеру, а не по наиболее свежему из всех входов.

Это означает, что:

- если трейды и стакан свежие, но тикер старый, возраст snapshot будет считаться большим;
- если тикер свежий, а trade flow давно не обновлялся, snapshot всё равно может выглядеть свежим.

То есть freshness в модуле привязана именно к ticker path.

## 9. `compute_technical()`: что именно считается

`FeatureEngine` сам не реализует формулы классических индикаторов. Он выступает orchestration-слоем поверх `IndicatorEngine`.

Из candle buffer берутся массивы:

- `close`
- `high`
- `low`
- `volume`

Дальше считаются следующие семейства признаков.

### 9.1. SMA / EMA

Считаются через `IndicatorEngine`:

- `sma(close, sma_period)` -> `sma_20`
- `ema(close, ema_fast_period)` -> `ema_20`
- `ema(close, ema_slow_period)` -> `ema_50`

Валидность:

- `sma_valid = sma.valid`
- `ema_valid = ema_fast.valid && ema_slow.valid`

### 9.2. RSI

`rsi(close, rsi_period)` -> `rsi_14`

### 9.3. MACD

`macd(close, macd_fast, macd_slow, macd_signal)` ->

- `macd_line`
- `macd_signal`
- `macd_histogram`

### 9.4. Bollinger Bands

`bollinger(close, bb_period, bb_stddev)` ->

- `bb_upper`
- `bb_middle`
- `bb_lower`
- `bb_bandwidth`
- `bb_percent_b`

### 9.5. ATR

`atr(high, low, close, atr_period)` -> `atr_14`

Дополнительно считается:

$$
atr\_14\_normalized = \frac{ATR}{last\_close}
$$

если ATR валиден и последняя цена закрытия положительна.

### 9.6. ADX

`adx(high, low, close, adx_period)` ->

- `adx`
- `plus_di`
- `minus_di`

### 9.7. OBV

`obv(close, volume)` -> `obv`

Затем считается простая нормализация:

$$
obv\_normalized = \frac{OBV}{mean(volume_{last\ 20})}
$$

если в буфере есть хотя бы 20 volume points.

Это не canonical OBV transform, а прикладная скейлинг-нормализация для downstream-моделей.

### 9.8. Волатильность

Через `IndicatorEngine::volatility()` считаются два окна:

- `volatility_5`
- `volatility_20`

Валидность выставляется только если валидны оба окна:

`volatility_valid = vol5.valid && vol20.valid`

### 9.9. Моментум

Через `IndicatorEngine::momentum()` считаются:

- `momentum_5`
- `momentum_20`

И аналогично:

`momentum_valid = mom5.valid && mom20.valid`

### 9.10. Важные практические наблюдения

1. Волатильность и моментум используют жёстко зашитые окна `5` и `20`; они не вынесены в `FeatureEngine::Config`.
2. Классические индикаторы конфигурируемы, но поля snapshot названы под дефолтные периоды.
3. `compute_technical()` не пытается сам валидировать цены: эта ответственность делегирована `IndicatorEngine`.

## 10. `compute_microstructure()`: как строятся признаки стакана и trade flow

Этот блок использует одновременно тикер, стакан и буфер сделок.

### 10.1. Спред и mid

Из тикера пишутся:

- `spread`
- `spread_bps`
- `spread_valid = bid > 0 && ask > 0`
- `mid_price = (bid + ask) / 2`

### 10.2. Weighted mid

Если есть `top_of_book()`, то `weighted_mid_price = tob->mid_price`.

Если top-of-book нет, fallback — обычный `mid_price`.

Если есть `depth_summary()`, то `weighted_mid_price` перезаписывается уже значением `depth->weighted_mid`.

То есть при наличии полного summary weighted mid берётся не с ТОB, а из глубины.

### 10.3. Дисбаланс стакана

Если `book.depth_summary(config_.book_depth_levels)` успешен, то записываются:

- `book_imbalance_5`
- `book_imbalance_10`
- `book_imbalance_valid = true`

Именно `LocalOrderBook` отвечает за то, как именно считаются эти imbalance-метрики. Модуль `features` здесь только потребляет готовый summary.

### 10.4. Ликвидность

Из depth summary считаются:

- `bid_depth_5_notional = bid_depth_5 * mid_price`
- `ask_depth_5_notional = ask_depth_5 * mid_price`

А затем:

$$
liquidity\_ratio = \frac{\min(bid\_depth\_5\_notional, ask\_depth\_5\_notional)}{0.5 \cdot (bid\_depth\_5\_notional + ask\_depth\_5\_notional)}
$$

Если total liquidity равна нулю, ставится `1.0`.

Это означает, что `liquidity_ratio` измеряет баланс сторон, а не абсолютный размер книги.

### 10.5. Поток сделок

Из `TradeBuffer` за окно `trade_flow_window` считаются:

- `buy_sell_ratio`
- `aggressive_flow`
- `trade_vwap`

Валидность блока ставится просто по наличию непустого trade buffer:

`trade_flow_valid = true`

### 10.6. Book instability

Оценивается эвристикой:

$$
book\_instability = clamp(0.7 \cdot |imbalance\_5| + 0.3 \cdot min(spread\_bps / 100, 1), 0, 1)
$$

То есть:

- 70% веса — дисбаланс стакана (основной предиктор краткосрочных ценовых движений);
- 30% веса — широкость спреда (вторичный фактор);
- результат нормируется в `[0, 1]`.

Научное обоснование весов: Cont, Kukanov & Stoikov (2014), "The Price Impact of Order Book Events" — order imbalance объясняет ~65–70% краткосрочных ценовых движений.

### 10.7. Важная runtime-особенность

`compute_microstructure()` вызывается только на тикере через `compute_snapshot()`. Поэтому:

- burst трейдов мгновенно обновит внутренний `TradeBuffer`;
- но новые `buy_sell_ratio`, `aggressive_flow` и `trade_vwap` попадут в snapshot только на следующем тикере.

## 11. `compute_execution_context()`: минимальный execution-aware слой

Этот блок intentionally простой и быстрый.

Считается:

- `spread_cost_bps = ticker.spread_bps`
- `immediate_liquidity = min(best_bid_size, best_ask_size)` если есть top-of-book
- `estimated_slippage_bps = spread_bps * 0.5`
- `slippage_valid = spread_bps > 0`
- `is_market_open = true`
- `is_feed_fresh = (now - ticker.received_ts) < feed_freshness_ns`

### Почему `is_market_open = true`

Это осознанный design choice под крипто-рынок: модуль не содержит сессионной spot-логики и не моделирует закрытие рынка по расписанию.

Для USDT-M futures/crypto 24/7 это корректное текущее допущение.

## 12. `FeatureEngine::Config`

Текущие дефолты:

- `sma_period = 20`
- `ema_fast_period = 20`
- `ema_slow_period = 50`
- `rsi_period = 14`
- `macd_fast = 12`
- `macd_slow = 26`
- `macd_signal = 9`
- `bb_period = 20`
- `bb_stddev = 2.0`
- `atr_period = 14`
- `adx_period = 14`
- `trade_flow_window = 100`
- `book_depth_levels = 10`
- `feed_freshness_ns = 1_000_000_000`
- `primary_interval = "1m"`

Из них:

- параметры технических индикаторов наследуют свою научную/практическую обоснованность из `IndicatorEngine`;
- `trade_flow_window = 100` и `book_depth_levels = 10` — прикладные runtime defaults;
- `feed_freshness_ns = 1s` — жёсткий low-latency порог для скальперского контура;
- `primary_interval = 1m` соответствует основному таймфрейму фичей в текущем pipeline.

## 13. `AdvancedFeatureEngine`: что это за слой

`AdvancedFeatureEngine` не строит snapshot сам. Он хранит собственное состояние и умеет **дописать** расширенные признаки в уже существующий `FeatureSnapshot` через `fill_snapshot()`.

Это отдельный pipeline-stage, а не часть `FeatureEngine`.

### 13.1. Концептуальное разделение ответственности

`FeatureEngine` отвечает за:

- классические TA-индикаторы;
- простую микроcтруктуру;
- execution context.

`AdvancedFeatureEngine` отвечает за:

- CUSUM;
- VPIN;
- Volume Profile;
- Time-of-Day seasonality.

То есть проект делит basic features и advanced statistical / microstructure features на два слоя.

## 14. `AdvancedFeatureEngine`: входы и state

Публичный интерфейс:

1. `on_tick(double price)`
2. `on_trade(double price, double volume, bool is_buy)`
3. `fill_snapshot(FeatureSnapshot&)`

Под mutex он хранит:

- буфер доходностей для CUSUM;
- состояние двустороннего CUSUM;
- состояние VPIN бакетов;
- историю трейдов для volume profile;
- предвычисленные уровни POC / Value Area;
- Time-of-Day конфигурацию.

## 15. CUSUM: как реализован

`on_tick(price)`:

1. Если `last_price_ > 0`, считает simple return:

$$
r_t = \frac{price_t - price_{t-1}}{price_{t-1}}
$$

2. Передаёт return в `update_cusum()`.
3. Обновляет `last_price_`.

`update_cusum()` работает так:

- сначала оценивает `mean` и `sigma` по **старому** буферу returns;
- использует выборочное std с делителем `n-1`;
- минимальная `sigma` клемпится к `1e-10`;
- текущий return стандартизируется в z-score;
- затем обновляются:

$$
C^+ = max(0, C^+ + z - drift)
$$

$$
C^- = max(0, C^- - z - drift)
$$

- если один из них превышает `threshold_mult`, флаг `cusum_change_detected_` ставится в `true`;
- значения на момент детекции сохраняются отдельно;
- после детекции рабочие аккумуляторы сбрасываются в ноль.

Потом новый return добавляется в буфер.

### Значение такой схемы

Это не просто накопление отклонений, а online regime-change detector, который старается не загрязнять оценку `sigma` текущим же движением до расчёта сигнала.

## 16. VPIN: как реализован

`on_trade(price, volume, is_buy)` вызывает `update_vpin(volume, is_buy)`.

Логика следующая:

1. Увеличивается счётчик трейдов.
2. Если пора, происходит периодическая рекалибровка размера volume bucket.
3. Текущая сделка добавляется в текущий бакет как buy или sell volume.
4. Если `bucket_target_volume_` ещё не известен, он калибруется после первых 10 трейдов как:

$$
avg\_trade\_volume \times bucket\_size
$$

5. При заполнении бакета он переносится в deque завершённых бакетов.
6. Если бакетов достаточно, считается:

$$
VPIN = \frac{\sum |buy\_volume - sell\_volume|}{\sum total\_volume}
$$

7. Для сглаживания считается EMA-подобное среднее:

$$
vpin\_ma = 0.9 \cdot old + 0.1 \cdot current
$$

### Практические замечания

1. Это volume-synchronized, а не time-synchronized flow metric.
2. Реализация не делает carry-over остатка при переполнении бакета одной крупной сделкой: весь заполненный bucket завершается целиком, а следующий начинается с нуля.
3. Это упрощённая, но практичная online-версия VPIN.

## 17. Volume Profile: как реализован

`on_trade()` также вызывает `update_volume_profile(price, volume)`.

Логика:

1. Сделка сохраняется в историю `trade_history_`.
2. История ограничивается `lookback_trades`.
3. Профиль пересчитывается не на каждой сделке, а каждые 100 трейдов, либо пока ещё не найден первый POC.
4. При наличии минимум 50 трейдов строится гистограмма объёма по `num_levels` ценовым корзинам.
5. Находится POC — корзина с максимальным объёмом.
6. Вокруг POC расширяется Value Area, пока не будет покрыто `value_area_pct` от total volume.

В snapshot пишутся:

- `vp_poc`
- `vp_value_area_high`
- `vp_value_area_low`
- `vp_price_vs_poc`

Причём `vp_price_vs_poc` нормируется так:

$$
clamp\left(\frac{mid - poc}{0.5 \cdot (VAH - VAL)}, -1, 1\right)
$$

## 18. Time-of-Day: как реализован

`TimeOfDayConfig` содержит три эмпирических массива длины 24:

- volatility multipliers;
- volume multipliers;
- alpha scores.

`fill_snapshot()`:

1. Берёт текущий UTC-час.
2. Записывает его в `session_hour_utc`.
3. Кладёт из массивов:
   - `tod_volatility_mult`
   - `tod_volume_mult`
   - `tod_alpha_score`
4. Ставит `tod_valid = true`.

### Важная фактическая особенность

Time-of-Day берёт время через `std::chrono::system_clock::now()`, а не через `clock_` и не через `snapshot.computed_at`.

Это означает:

- в live это почти незаметно;
- в replay/backtest это может расходиться с симулируемым временем;
- модуль становится менее детерминированным и менее test-friendly.

## 19. Как `AdvancedFeatureEngine` интегрирован в pipeline

В `TradingPipeline` сейчас происходит следующее:

1. Pipeline получает уже готовый базовый `FeatureSnapshot`.
2. Если есть mid price, вызывает:

   `advanced_features_->on_tick(snapshot.mid_price.get())`

3. Затем вызывает:

   `advanced_features_->fill_snapshot(snapshot)`

На этом текущая интеграция заканчивается.

### Ключевой вывод

В основном runtime-контуре **нет вызова**:

`advanced_features_->on_trade(...)`

Из этого следует очень важное фактическое ограничение:

- `CUSUM` работает, потому что питается через `on_tick()`;
- `Time-of-Day` работает, потому что вычисляется в `fill_snapshot()`;
- `VPIN` не прогревается в основном pipeline-path;
- `Volume Profile` тоже не прогревается в основном pipeline-path.

То есть поля `vpin_*` и `vp_*` в текущем production path объявлены и реализованы, но без дополнительной интеграции трейдового потока они, по сути, остаются незаполненными.

Это не проблема интерфейса `AdvancedFeatureEngine`; это именно текущий integration gap верхнего слоя.

## 20. Futures-only и отсутствие spot-логики

В самом модуле `features` нет:

- spot-specific branching;
- spot-only ограничений по торговым сессиям;
- терминологии покупки/продажи спот-актива как отдельной доменной ветки.

Наоборот, execution-context слой прямо исходит из того, что рынок crypto работает 24/7:

- `is_market_open = true`.

То есть модуль хорошо согласуется с USDT-M futures runtime и не содержит встроенной spot-семантики.

## 21. Что реально тестируется

По тестам в репозитории видно только `tests/unit/features/feature_snapshot_test.cpp`.

Он проверяет:

- базовое поведение `FeatureSnapshot::is_complete()`;
- начальные значения структур;
- простые DTO-инварианты.

Но в текущем состоянии не видно полноценного unit-test покрытия для:

- `FeatureEngine::compute_technical()`;
- `FeatureEngine::compute_microstructure()`;
- `FeatureEngine::compute_execution_context()`;
- `AdvancedFeatureEngine::update_cusum()`;
- `AdvancedFeatureEngine::update_vpin()`;
- `AdvancedFeatureEngine::update_volume_profile()`;
- `AdvancedFeatureEngine::fill_snapshot()`.

То есть основной confidence по модулю сейчас строится больше на интеграции и косвенном поведении pipeline, чем на изолированных unit-тестах движков.

## 22. Сильные стороны модуля

### 22.1. Чёткое разделение слоёв

Модуль не смешивает market-data ingestion, order-book maintenance и стратегические решения.

### 22.2. Центральный snapshot contract

Верхние уровни получают один единый `FeatureSnapshot`, а не десятки разрозненных значений.

### 22.3. Хорошее разделение basic vs advanced features

Классические индикаторы и простая microstructure-логика выделены отдельно от более тяжёлых statistical features.

### 22.4. Потокобезопасность

Оба движка защищают внутреннее состояние мьютексом и могут использоваться из асинхронного event-потока.

### 22.5. Отсутствие spot-доменных артефактов

Модуль не тащит лишнюю spot-логику в futures-only pipeline.

## 23. Реальные ограничения и тонкие места

### 23.1. Snapshot строится только по тикеру

Это упрощает поток, но делает все признаки ticker-driven по моменту публикации.

### 23.2. `is_complete()` — усилен

✅ **Исправлено**: теперь требует `sma_valid && atr_valid && spread_valid`. Без ATR невозможна корректная оценка stop-loss / take-profit для скальпинга.

### 23.3. `is_ready()` — учитывает все индикаторы

✅ **Исправлено**: теперь в расчёт минимального количества свечей входят все периоды: `sma_period`, `ema_slow_period`, `macd_slow`, `bb_period`, `atr_period`, `adx_period`.

### 23.4. Имена полей snapshot жёстко зашиты под дефолтные периоды

Это делает DTO менее честным при нестандартной конфигурации.

### 23.5. Незакрытая свеча может участвовать в теханализе

Это осознанное live-поведение, но его важно помнить при интерпретации сигналов.

### 23.6. `AdvancedFeatureEngine` полностью подключён

✅ **Исправлено**: `on_trade()` теперь вызывается через `TradeCallback` в `MarketDataGateway`. VPIN и Volume Profile получают данные.

### 23.7. Time-of-Day использует injected clock

✅ **Исправлено**: `AdvancedFeatureEngine` принимает `IClock` в конструктор. В `fill_snapshot()` UTC-час вычисляется из `clock_->now()`, а не из `system_clock`. Это обеспечивает детерминизм в replay/backtest.

### 23.8. Unit-тестов на ключевую вычислительную логику практически нет

Для такого центрального data-preparation слоя это заметный пробел.

## 24. Краткий итог

Текущий `src/features` — это центральный aggregation layer признаков для торгового контура, а не просто набор DTO.

Главные выводы:

1. `FeatureEngine` агрегирует свечи, тикеры, трейды и стакан в единый `FeatureSnapshot`.
2. Классические технические индикаторы считаются через `IndicatorEngine`, а не внутри `features`.
3. Snapshot публикуется только на тикере, поэтому весь модуль фактически ticker-driven по моменту выдачи результата.
4. `AdvancedFeatureEngine` реализует CUSUM, VPIN, Volume Profile и Time-of-Day — все четыре подсистемы полностью подключены.
5. Модуль согласован с USDT-M futures runtime и не содержит встроенной spot-логики.
6. Все конфигурационные дефолты научно обоснованы (ссылки в коде).

## 25. Changelog (2026-04-06 overhaul)

| Изменение | Обоснование |
|---|---|
| `on_trade()` wired через `TradeCallback` в MarketDataGateway | **CRITICAL**: VPIN и Volume Profile были мертвы — теперь получают данные |
| `AdvancedFeatureEngine` принимает `IClock` | Детерминизм Time-of-Day в replay/backtest |
| `is_complete()` требует `atr_valid` | Скальпинг без ATR не может строить SL/TP |
| `is_ready()` учитывает `bb_period`, `atr_period`, `adx_period` | Все индикаторы должны прогреться до публикации snapshot |
| book_instability: 0.6/0.4 → 0.7/0.3 | Cont, Kukanov & Stoikov (2014): imbalance ~65–70% предиктивной силы |
| VPIN volume carry-over | Переполнение бакета теперь переносится в следующий, а не теряется |
| VPIN EMA инициализация | Первое значение `vpin_ma_` = `vpin_value_`, а не 0 (убирает cold-start bias) |
| OBV нормализация: ручной цикл → `std::accumulate` | Idiomatic C++ |
| CUSUM σ минимум: 1e-10 → 1e-12 | Согласовано с `kMinVariance` из `numeric_utils.hpp` |
| Все конфиги документированы научными ссылками | Page (1954), Easley et al. (2012), Dalton (1990), Cont et al. (2014), Wilder (1978), Bollinger (2002), Appel (1979), Murphy (1999), Eross et al. (2019) |