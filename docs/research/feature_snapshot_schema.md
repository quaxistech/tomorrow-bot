# Схема FeatureSnapshot — Tomorrow Bot Фаза 2

**Расположение:** `src/features/feature_snapshot.hpp`  
**Пространство имён:** `tb::features`

Снимок признаков (`FeatureSnapshot`) — центральный объект передачи данных
между слоем рыночных данных и слоем принятия решений. Вычисляется на каждый
тикер или по явному запросу.

---

## Корневые поля

| Поле | Тип | Описание |
|---|---|---|
| `symbol` | `tb::Symbol` | Торговый символ (напр. `BTCUSDT`) |
| `computed_at` | `tb::Timestamp` | Время вычисления снимка (нс) |
| `market_data_age_ns` | `tb::Timestamp` | Возраст рыночных данных (нс) |
| `last_price` | `tb::Price` | Последняя цена сделки |
| `mid_price` | `tb::Price` | Средняя цена стакана ((bid+ask)/2) |
| `book_quality` | `BookQuality` | Качество стакана заявок |

### `is_complete()`

```cpp
[[nodiscard]] bool is_complete() const noexcept;
```

Возвращает `true` если снимок пригоден для принятия решений:
`technical.sma_valid && microstructure.spread_valid`.

---

## TechnicalFeatures — технические индикаторы

Вычисляются на основе закрытых цен свечей (candle close).

### Скользящие средние

| Поле | Тип | Описание |
|---|---|---|
| `sma_20` | `double` | Simple Moving Average, период 20 |
| `ema_20` | `double` | Exponential Moving Average, период 20 |
| `ema_50` | `double` | Exponential Moving Average, период 50 |
| `sma_valid` | `bool` | `true` если SMA вычислена |
| `ema_valid` | `bool` | `true` если EMA вычислена |

### RSI

| Поле | Тип | Описание |
|---|---|---|
| `rsi_14` | `double` | Relative Strength Index (0–100), период 14 |
| `rsi_valid` | `bool` | `true` если RSI вычислен |

### MACD

| Поле | Тип | Описание |
|---|---|---|
| `macd_line` | `double` | Линия MACD (fast EMA − slow EMA) |
| `macd_signal` | `double` | Сигнальная линия (EMA от MACD) |
| `macd_histogram` | `double` | Гистограмма (MACD − signal) |
| `macd_valid` | `bool` | `true` если MACD вычислен |

### Bollinger Bands

| Поле | Тип | Описание |
|---|---|---|
| `bb_upper` | `double` | Верхняя полоса |
| `bb_middle` | `double` | Средняя линия (SMA 20) |
| `bb_lower` | `double` | Нижняя полоса |
| `bb_bandwidth` | `double` | Ширина полос ((upper−lower)/middle) |
| `bb_percent_b` | `double` | Положение цены внутри полос (0.0–1.0) |
| `bb_valid` | `bool` | `true` если BB вычислены |

### ATR (Average True Range)

| Поле | Тип | Описание |
|---|---|---|
| `atr_14` | `double` | ATR период 14 (в единицах цены) |
| `atr_14_normalized` | `double` | ATR / close × 100 (в процентах) |
| `atr_valid` | `bool` | `true` если ATR вычислен |

### ADX (Average Directional Index)

| Поле | Тип | Описание |
|---|---|---|
| `adx` | `double` | Индекс силы тренда (0–100) |
| `plus_di` | `double` | +DI (направление вверх) |
| `minus_di` | `double` | −DI (направление вниз) |
| `adx_valid` | `bool` | `true` если ADX вычислен |

### OBV (On-Balance Volume)

| Поле | Тип | Описание |
|---|---|---|
| `obv` | `double` | Накопленный объём (сырой) |
| `obv_normalized` | `double` | OBV нормализованный (z-score) |
| `obv_valid` | `bool` | `true` если OBV вычислен |

### Волатильность и моментум

| Поле | Тип | Описание |
|---|---|---|
| `volatility_5` | `double` | Реализованная волатильность за 5 периодов |
| `volatility_20` | `double` | Реализованная волатильность за 20 периодов |
| `volatility_valid` | `bool` | `true` если волатильность вычислена |
| `momentum_5` | `double` | Моментум за 5 периодов (close/close[−5]−1) |
| `momentum_20` | `double` | Моментум за 20 периодов |
| `momentum_valid` | `bool` | `true` если моментум вычислен |

---

## MicrostructureFeatures — микроструктура рынка

Вычисляются на основе стакана заявок и потока сделок.

### Спред

| Поле | Тип | Описание |
|---|---|---|
| `spread` | `double` | Абсолютный спред (ask − bid) |
| `spread_bps` | `double` | Спред в базисных пунктах (spread/mid × 10000) |
| `spread_valid` | `bool` | `true` если стакан имеет хотя бы один бид и аск |

### Дисбаланс стакана

| Поле | Тип | Описание |
|---|---|---|
| `book_imbalance_5` | `double` | Дисбаланс топ-5 уровней (−1.0 .. +1.0) |
| `book_imbalance_10` | `double` | Дисбаланс топ-10 уровней (−1.0 .. +1.0) |
| `book_imbalance_valid` | `bool` | `true` если стакан достаточно глубок |

> **Формула:** `(bid_vol − ask_vol) / (bid_vol + ask_vol)`  
> +1.0 = весь объём на стороне бид (давление покупателей)  
> −1.0 = весь объём на стороне аск (давление продавцов)

### Средняя цена

| Поле | Тип | Описание |
|---|---|---|
| `mid_price` | `double` | (best_bid + best_ask) / 2 |
| `weighted_mid_price` | `double` | Взвешенная по объёму средняя цена стакана |

### Поток сделок

| Поле | Тип | Описание |
|---|---|---|
| `buy_sell_ratio` | `double` | Объём покупок / объём продаж (скользящее окно) |
| `aggressive_flow` | `double` | Доля агрессивных ордеров (0.0–1.0) |
| `trade_vwap` | `double` | VWAP потока сделок |
| `trade_flow_valid` | `bool` | `true` если данные потока накоплены |

### Ликвидность

| Поле | Тип | Описание |
|---|---|---|
| `bid_depth_5_notional` | `double` | Объём бид в деньгах (топ 5 уровней) |
| `ask_depth_5_notional` | `double` | Объём аск в деньгах (топ 5 уровней) |
| `liquidity_ratio` | `double` | bid_depth / ask_depth |
| `liquidity_valid` | `bool` | `true` если стакан достаточно глубок |

### Нестабильность стакана

| Поле | Тип | Описание |
|---|---|---|
| `book_instability` | `double` | Мера изменчивости стакана (частота обновлений) |
| `instability_valid` | `bool` | `true` если нестабильность вычислена |

---

## ExecutionContextFeatures — контекст исполнения

Оценивает условия для открытия позиции прямо сейчас.

| Поле | Тип | Описание |
|---|---|---|
| `spread_cost_bps` | `double` | Стоимость пересечения спреда в bps |
| `immediate_liquidity` | `double` | Доступный объём по текущим ценам |
| `estimated_slippage_bps` | `double` | Ожидаемое проскальзывание в bps |
| `slippage_valid` | `bool` | `true` если оценка проскальзывания доступна |
| `is_market_open` | `bool` | `true` если торговая сессия открыта (по умолчанию `true`) |
| `is_feed_fresh` | `bool` | `true` если последнее обновление пришло < N мс назад |

---

## Примечания

- Все `double`-поля инициализируются `0.0` по умолчанию.
- Все `bool`-поля (`*_valid`, `is_*`) инициализируются `false`, кроме `is_market_open = true`.
- Перед использованием поля всегда проверяйте соответствующий флаг `*_valid`.
- `FeatureSnapshot` — value-type, передаётся по значению или `const&`.
