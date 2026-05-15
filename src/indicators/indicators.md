# Подробный разбор модуля indicators

Временный аналитический документ.

Источник разбора: текущая реализация `src/indicators`, её прямые потребители (`features`, `pipeline`, `strategy`) и связанный верхний слой `features/advanced_features` на момент 2026-04-06.

Документ описывает фактическое поведение кода после актуального рефакторинга, а не только исходный архитектурный замысел.

## 1. Роль модуля в системе

`indicators` — это специализированный вычислительный модуль для технических индикаторов и производных статистик по уже подготовленным временным рядам.

Он **не** занимается:

- получением market data;
- хранением order book;
- нормализацией биржевых сообщений;
- агрегацией trade feed;
- lifecycle управления позицией;
- risk-решениями;
- execution.

Его реальная роль:

```text
MarketDataGateway
	-> FeatureEngine / pipeline HTF buffers
	-> IndicatorEngine
	-> TechnicalFeatures / HTF trend filter
	-> Strategy / execution / pipeline gates
```

То есть это не orchestration-слой, а математическое ядро технической аналитики.

## 2. Состав модуля

В папке `src/indicators` сейчас находятся:

| Файл | Назначение |
|---|---|
| `src/indicators/CMakeLists.txt` | сборка статической библиотеки `tb_indicators` |
| `src/indicators/indicator_types.hpp` | типы результатов индикаторов |
| `src/indicators/indicator_engine.hpp` | публичный API `IndicatorEngine` |
| `src/indicators/indicator_engine.cpp` | реализация индикаторов |
| `src/indicators/indicators.md` | этот временный аналитический документ |

Сборка:

```cmake
add_library(tb_indicators STATIC indicator_engine.cpp)
target_include_directories(tb_indicators PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_indicators PUBLIC tb_common tb_logging tb_buffers)
```

Основные зависимости:

- `common/numeric_utils.hpp` — численная безопасность;
- `logging/logger.hpp` — предупреждения при плохом входе;
- потребители сверху: `FeatureEngine`, `TradingPipeline`, тесты.

## 3. Что изменилось по сравнению с предыдущим состоянием

Модуль уже доведён до существенно более чистого состояния:

- убран мёртвый интерфейс `IIndicator`;
- убран неиспользуемый `RollingState`;
- убраны неиспользуемые статусы `Stale` и `WarmingUp`;
- убрана лишняя двухступенчатая схема `public -> _builtin`;
- в модуль перенесены `volatility()` и `momentum()`, которые раньше считались вне него в `FeatureEngine`;
- `TradingPipeline` больше не дублирует руками EMA/RSI/MACD/ADX для HTF-анализа, а использует `IndicatorEngine`.

Это означает, что модуль теперь действительно стал единым центром классических индикаторных расчётов в проекте.

## 4. Публичный API

Главный объект — `IndicatorEngine`.

Он предоставляет следующие расчёты.

### 4.1. Базовые технические индикаторы

1. `sma(prices, period)`
2. `ema(prices, period)`
3. `rsi(prices, period)`
4. `macd(prices, fast, slow, signal)`
5. `bollinger(prices, period, stddev)`
6. `atr(high, low, close, period)`
7. `adx(high, low, close, period)`
8. `obv(prices, volumes)`
9. `vwap(high, low, close, volume)`

### 4.2. Расширенные индикаторы и статистики

10. `rolling_vwap(high, low, close, volume, window, band_stddev)`
11. `rate_of_change(prices, period)`
12. `z_score(prices, period)`
13. `volatility(prices, period)`
14. `momentum(prices, period)`

Итог: по текущему состоянию модуль даёт уже **14 вычислений**, если считать расширенные статистики как часть indicator API.

## 5. Научная и практическая обоснованность дефолтов

В хедере модуля дефолтные параметры прямо документированы как canonical defaults, а не произвольные значения.

### Используемые базовые дефолты

- `RSI(14)` — Wilder (1978)
- `ATR(14)` — Wilder (1978)
- `ADX(14)` — Wilder (1978)
- `MACD(12,26,9)` — Appel (1979)
- `Bollinger(20, 2.0)` — классическая настройка Bollinger Bands
- `EMA` — стандартный smoothing coefficient $\alpha = 2/(n+1)$

### Что это значит practically для вашего бота

Для USDT-M scalp / intraday торговых систем это нормальные профессиональные defaults, потому что:

- они являются рыночным стандартом и упрощают сопоставление сигналов с практикой индустрии;
- они не являются случайными magic numbers;
- они уже достаточно устойчивы для 1m рабочего таймфрейма и 1H HTF-фильтра.

Отдельно важно: модуль `indicators` сам **не содержит спот-логики и спот-терминологии**.

## 6. Формат результатов

Все функции возвращают не только число, но и metadata:

- `valid`
- `status`
- `sample_count`
- `warmup_remaining`

Для сложных индикаторов используются специальные result-структуры:

- `MacdResult`
- `BollingerResult`
- `AdxResult`
- `VwapResult`

Статусы теперь лаконичные и реальные:

- `Ok`
- `InsufficientData`
- `InvalidInput`

Это лучше прежнего состояния, где типы были богаче фактической логики.

## 7. Численная устойчивость

Модуль использует `common/numeric_utils.hpp` и потому рассчитан на production, а не только на учебные идеальные ряды.

Основные защитные механизмы:

- `validate_price_series()` — отбрасывает пустые/невалидные/нефинитные цены;
- `is_valid_volume()` — отбрасывает отрицательные и нефинитные объёмы;
- `safe_div()` — защищает от деления на почти ноль;
- `safe_sqrt()` — защищает от отрицательного аргумента;
- `kEpsilon = 1e-9` — общий порог устойчивости.

Это особенно важно для крипто-фьючерсного runtime, где:

- бывают поломанные payloads;
- бывают редкие нулевые объёмы/плохие свечи;
- нельзя позволять индикаторному слою тихо порождать NaN.

## 8. Подробно по каждому индикатору

### 8.1. SMA

Считает простое среднее последних `period` значений:

$$
SMA_t = \frac{1}{n}\sum_{i=t-n+1}^{t} P_i
$$

Поведение:

- `period <= 0` -> `InvalidInput`
- недостаточно баров -> `InsufficientData`
- невалидная серия -> `InvalidInput`

Реализация корректна и минималистична.

### 8.2. EMA

Использует seed как SMA первых `period` значений и затем стандартное экспоненциальное сглаживание:

$$
EMA_t = \alpha P_t + (1-\alpha) EMA_{t-1}, \quad \alpha = \frac{2}{n+1}
$$

Это canonical реализация.

### 8.3. RSI

Реализован по Wilder.

Формула:

$$
RSI = 100 - \frac{100}{1 + RS}
$$

где

$$
RS = \frac{AvgGain}{AvgLoss}
$$

Особенность: при практически нулевом `avg_loss` RSI принудительно ставится в `100`, что является нормальным защитным поведением.

### 8.4. MACD

Использует классическую тройку параметров `12/26/9`.

Считает:

- fast EMA;
- slow EMA;
- `macd_line = EMA_fast - EMA_slow`;
- signal EMA по MACD-ряду;
- `histogram = macd - signal`.

Это корректная профессиональная реализация.

### 8.5. Bollinger Bands

Считает:

- среднюю линию;
- верхнюю/нижнюю границы;
- `bandwidth`;
- `%B`.

Текущая реализация использует дисперсию по окну с делением на `period`, то есть population-style variance. Для Bollinger Bands это допустимо и широко используется.

### 8.6. ATR

Считает True Range и далее Wilder smoothing:

$$
TR_t = \max(H_t - L_t, |H_t - C_{t-1}|, |L_t - C_{t-1}|)
$$

Это правильная формула для фьючерсной intraday-волатильности и стоповых расстояний.

### 8.7. ADX

Реализация ADX корректно использует:

- `+DM`
- `-DM`
- `TR`
- сглаживание Wilder
- `+DI`, `-DI`, `DX`, `ADX`

Это один из самых важных индикаторов для фильтрации трендовых/нетрендовых условий в ваших scalp-сетапах, и он в модуле реализован правильно.

### 8.8. OBV

Классический On-Balance Volume.

Поведение:

- цена вверх -> объём добавляется;
- цена вниз -> объём вычитается;
- flat -> OBV не меняется.

### 8.9. VWAP

Теперь `vwap()` принимает параметр `window` (по умолчанию 50), что соответствует ~1 часу на 1-минутных свечах.

Внутренне `vwap()` делегирует вычисление `rolling_vwap()` с `band_stddev=0` для устранения дублирования кода (~20 строк TP*volume суммирования были идентичны в обеих функциях).

Дефолт `window=50` обоснован как стандартный intraday VWAP anchor для mean-reversion скальпинга (Dacorogna et al., 2001).

### 8.10. rolling_vwap

Это полная rolling-window версия VWAP с bands:

- `vwap`
- `upper_band`
- `lower_band`
- `cumulative_volume`

Дефолт `band_stddev=1.0` — 1σ bands используются в институциональном execution для идентификации mean-reversion зон (Kissell, 2014).

`vwap()` теперь делегирует сюда, так что вычислительная логика централизована в одном месте.

### 8.11. ROC

`rate_of_change()` возвращает процентное изменение:

$$
ROC = \left(\frac{P_t - P_{t-n}}{P_{t-n}}\right) \cdot 100
$$

То есть это **проценты**, а не доля.

### 8.12. Z-Score

Считает отклонение текущей цены от среднего окна в единицах стандартного отклонения:

$$
Z = \frac{P_t - \mu}{\sigma}
$$

Использует **population std** (делитель N, не N-1): окно рассматривается как reference set, а не как выборка из большей популяции. Это осознанное архитектурное решение (в Bollinger Bands используется тот же подход).

Теперь требует `period >= 2` (ранее допускался period=1, при котором σ=0 и z-score всегда возвращал бессмысленный 0).

Дефолт `period=20` совпадает с lookback Bollinger Bands (Bollinger, 2002).

Если дисперсия почти нулевая, через `safe_div()` вернётся безопасный `0.0`.

### 8.13. Volatility

Это уже важное изменение нового состояния модуля.

Теперь `volatility()` официально живёт в `IndicatorEngine`, а не считается локальным helper-ом в `FeatureEngine`.

Реализация:

- берёт лог-доходности;
- считает их среднее;
- считает выборочную дисперсию с поправкой Бесселя (`n-1`);
- возвращает sample standard deviation.

Это математически более корректно, чем прежняя локальная ad-hoc реализация, и соответствует практикам финансовой статистики.

### 8.14. Momentum

Теперь `momentum()` тоже живёт в модуле:

$$
Momentum = \frac{P_t - P_{t-n}}{P_{t-n}}
$$

Важно: это **доля**, а не проценты.

То есть:

- `momentum = 0.05` означает `+5%`
- `ROC = 5.0` означает `+5%`

Обе функции нужны, но семантически различаются.

## 9. Где модуль реально используется

### 9.1. `FeatureEngine`

`FeatureEngine::compute_technical()` сейчас использует из `IndicatorEngine`:

- SMA
- EMA fast/slow
- RSI
- MACD
- Bollinger Bands
- ATR
- ADX
- OBV
- volatility 5/20
- momentum 5/20

Это как раз тот набор, который ты отдельно перечислил как критически важный для `market_data` / `features` слоя.

Значит после рефакторинга ответ на этот конкретный вопрос такой:

**Да, все перечисленные тобой индикаторы теперь действительно сосредоточены в модуле `indicators`.**

### 9.2. `TradingPipeline`

HTF-фильтр в `TradingPipeline` теперь тоже использует `IndicatorEngine`, а не собственный hand-rolled код.

Через него считаются:

- HTF EMA20
- HTF EMA50
- HTF RSI14
- HTF MACD histogram
- HTF ADX14

Это важный architectural win: дублирование расчётной логики удалено.

## 10. Что ещё остаётся вне модуля

Нужно честно отделять классические индикаторы от продвинутых market microstructure / statistical features.

Вне `src/indicators` всё ещё живут:

- `CUSUM`
- `VPIN`
- `Volume Profile`
- `Time-of-Day`

Они находятся в `src/features/advanced_features.*`.

Это не означает неполноту `IndicatorEngine`.
Это означает, что проект архитектурно делит:

- классические TA-индикаторы;
- продвинутые статистические / execution-aware признаки.

## 11. Все ли необходимые проекту индикаторы реализованы?

Ответ надо дать на двух уровнях.

### 11.1. Для базового technical layer, который нужен скальпинговым стратегиям USDT-M

Да, реализовано всё необходимое:

- SMA
- EMA fast/slow
- RSI
- MACD
- Bollinger Bands
- ATR
- ADX
- OBV
- volatility 5/20
- momentum 5/20

И это уже реально используется в `FeatureEngine` и `TradingPipeline`.

### 11.2. Для проекта в широком смысле

Если считать вообще все сигналы, которые влияют на execution/strategy decisions, то часть расширенных признаков остаётся вне модуля:

- `CUSUM`
- `VPIN`
- `Volume Profile`
- `Time-of-Day`

То есть:

- **классические индикаторы проекта реализованы полностью**;
- **все перечисленные тобой indicators для `market_data/features` уже находятся в этом модуле**;
- **не все project-wide advanced signals находятся здесь**, но это уже другая категория признаков, а не missing technical indicators.

## 12. Тестовое покрытие

На текущий момент тесты покрывают:

- SMA
- EMA
- RSI
- MACD
- Bollinger Bands
- ATR
- ADX
- OBV
- VWAP
- volatility
- momentum
- ROC
- Z-Score

Плюс smoke/integration coverage.

После последнего обновления полный прогон даёт:

- `401/401 tests passed`

Это сильный сигнал, что рефакторинг не сломал downstream-поведение.

## 13. Сильные стороны модуля

### 13.1. Единый центр технической аналитики

Раньше часть нужных проекту индикаторов была размазана по `FeatureEngine`, а HTF-расчёты вручную дублировались в pipeline.

Теперь вычислительное ядро централизовано.

### 13.2. Убрано мёртвое API

Удалены:

- `IIndicator`
- `RollingState`
- мёртвые статусы
- `_builtin`-дублирование

То есть модуль стал чище и ближе к реальному runtime.

### 13.3. Научно обоснованные defaults

Параметры не выглядят выдуманными — они соответствуют canonical market defaults.

### 13.4. Numerical safety

Модуль устойчив к битому входу и не порождает NaN-хаос при типовых edge cases.

## 14. Оставшиеся ограничения

### 14.1. ~~`vwap()` по названию шире, чем по факту~~ (ИСПРАВЛЕНО)

Теперь `vwap()` принимает configurable `window`, делегирует в `rolling_vwap()`, и не содержит дублированного кода.

### 14.2. Advanced microstructure features всё ещё вне модуля

Если когда-то захочется сделать `indicators` единственным центром **всех** quantitative signals, то туда пришлось бы переносить уже не только technical indicators, но и часть `advanced_features`. Сейчас это ещё не так.

### 14.3. У модуля нет streaming/incremental API

Сейчас все вычисления пакетные по векторам. Для текущего проекта это приемлемо, но для ultra-low-latency incremental path можно было бы в будущем строить stateful indicator objects.

## 15. Краткий итог

Текущий `src/indicators` теперь находится в хорошем профессиональном состоянии.

Главные выводы:

1. модуль больше не содержит мёртвых abstraction-слоёв;
2. в него перенесены `volatility` и `momentum`, которые раньше были разбросаны по проекту;
3. ручное дублирование EMA/RSI/MACD/ADX в pipeline убрано;
4. все перечисленные тобой базовые индикаторы для `market_data/features` теперь реально находятся в этом модуле;
5. классический набор индикаторов, необходимый для USDT-M scalp-стратегий проекта, реализован полностью;
6. advanced statistical/microstructure signals (`CUSUM`, `VPIN`, `Volume Profile`, `Time-of-Day`) остаются в другом модуле, и это сейчас архитектурное разделение ответственности, а не дефект `indicators`.

Если описать модуль одной фразой: это уже не просто набор формул, а единый и достаточно зрелый technical-analysis backend для всего основного trading path проекта.
