# Поток данных — Tomorrow Bot

## Архитектура потока данных

### 1. Выбор торговых пар (PairScanner)

```
Bitget REST API: /api/v2/spot/public/symbols + /api/v2/spot/market/tickers
    → PairScanner.scan()
    → фильтрация (объём > 500K, спред < 50bps, статус = online)
    → скоринг (объём 40% + волатильность 30% + ATR 20% + ликвидность 10%)
    → top-N пар (по умолчанию 5)
    → выбор лучшей пары → передача в TradingPipeline
    → 24-часовая ротация (повторный скан и возможная смена пары)
```

### 2. Bootstrap: загрузка исторических данных

```
Bitget REST API: /api/v2/spot/market/candles
    → bootstrap_historical_candles(): 200 × 1m свечей (≈3.3 часа)
        → FeatureEngine.on_candle() для каждой свечи
        → прогрев технических индикаторов (SMA, EMA, RSI, MACD, BB, ATR, ADX)
    → bootstrap_htf_candles(): 200 × 1h свечей (≈8.3 дня)
        → compute_htf_trend()
        → вычисление HTF: EMA20, EMA50, RSI14, MACD(12/26/9), ADX14
        → определение тренда: direction (-1/0/+1), strength (0.0-1.0)
```

### 3. Реальное время (WebSocket)

```
Bitget WS → BitgetWsClient → RawWsMessage
→ BitgetNormalizer → NormalizedEvent (Ticker | Trade | OrderBook | Candle)
→ LocalOrderBook (только события OrderBook)
→ FeatureEngine.on_ticker / on_trade / on_candle
→ FeatureSnapshot (вычисляется по запросу или при каждом тикере)
→ Market Readiness Gate → HTF Trend Filter → Стратегии
```

---

## Компоненты

### 0. PairScanner (новый)

**Расположение:** `src/pair_scanner/`

Автоматически сканирует все доступные пары на Bitget и выбирает лучшие для торговли.
Использует REST API для получения списка инструментов и текущих тикеров.

**Алгоритм скоринга:**
- Объём 24ч (вес 40%) — высокая ликвидность = узкий спред, быстрое исполнение
- Волатильность (вес 30%) — достаточный диапазон для прибыли
- ATR (вес 20%) — абсолютный диапазон движения
- Ликвидность (вес 10%) — глубина стакана

**Фильтры:**
- `min_volume_usd` — минимальный суточный объём (по умолчанию 500,000 USDT)
- `max_spread_bps` — максимальный спред (по умолчанию 50 bps)
- Только пары со статусом `online`

**Режимы:** `auto` (автоскоринг), `manual` (ручной список), `hybrid` (авто + белый список)  
**Ротация:** каждые 24 часа (конфигурируемо через `pair_selection.rotation_hours`)

**Выход:** `PairScanResult` — ранжированный список пар с оценками

---

### 0b. HTF Trend Analysis (новый)

**Расположение:** логика в `src/pipeline/trading_pipeline.cpp`

Загружает 200 часовых свечей (≈8 дней) через Bitget REST API и вычисляет
комплексный анализ тренда на старшем таймфрейме (HTF).

**Вычисляемые индикаторы:**
- **EMA 20** (1h) — быстрая скользящая средняя
- **EMA 50** (1h) — медленная скользящая средняя
- **RSI 14** (1h) — индекс относительной силы
- **MACD** (12/26/9, 1h) — линия, сигнал, гистограмма
- **ADX 14** (1h) — индекс направленного движения

**Определение тренда:**
- `htf_trend_direction_`: +1 (бычий, EMA20 > EMA50 + ADX > 20), -1 (медвежий), 0 (боковик)
- `htf_trend_strength_`: 0.0-1.0, композит из ADX (50%), EMA gap (30%), RSI deviation (20%)

**Использование:**
- Market Readiness Gate: блокирует торговлю при невалидных HTF данных
- HTF Trend Filter: блокирует BUY в даунтренде, SELL в аптренде
- Стратегии используют LTF (1m) данные, но HTF даёт контекст

---

### 1. BitgetWsClient

**Расположение:** `src/exchange/bitget/bitget_ws_client.hpp`

Устанавливает и поддерживает WebSocket-соединение с биржей Bitget (v2 API).
Получает сырые текстовые кадры и оборачивает их в `RawWsMessage` с временной меткой
получения в наносекундах (`received_ns`). Определяет тип сообщения по полю `channel`
и записывает его в `WsMsgType` (Ticker, Trade, OrderBook, Candle, …).

**Выход:** `tb::exchange::bitget::RawWsMessage`

---

### 2. BitgetNormalizer

**Расположение:** `src/normalizer/normalizer.hpp`

Преобразует биржеспецифичный JSON (`RawWsMessage`) во внутренние типы данных.
Безопасно обрабатывает битые и пустые сообщения: при ошибке парсинга событие
просто отбрасывается, исключение не пробрасывается наружу.

Распознаёт и нормализует:
- **Ticker** → `NormalizedTicker` (last price, bid, ask, spread, volume 24h)
- **Trade** → `NormalizedTrade` (цена, объём, сторона, агрессивность)
- **OrderBook** → `NormalizedOrderBook` (snapshot или delta, sequence id)
- **Candle** → `NormalizedCandle` (OHLCV, флаг закрытия)

Каждое событие упаковывается в `NormalizedEvent` (std::variant) и передаётся
в callback `NormalizedEventCallback`.

**Выход:** `tb::normalizer::NormalizedEvent`

---

### 3. LocalOrderBook

**Расположение:** `src/order_book/order_book.hpp`

Хранит локальную копию биржевого стакана заявок для одного символа.
Поддерживает два режима обновления:
- **Snapshot** — полная замена стакана, устанавливает quality = Valid
- **Delta** — инкрементальное обновление; при несоответствии sequence_id
  переходит в состояние Desynced и отклоняет дельту

Состояния качества (`BookQuality`):
- `Uninitialized` — снимок ещё не получен
- `Valid` — данные актуальны
- `Stale` — данные устарели по времени
- `Desynced` — обнаружен пропуск последовательности
- `Resyncing` — ожидание нового снимка

Предоставляет:
- `top_of_book()` — лучший бид/аск, спред, средняя цена
- `depth_summary(levels)` — глубина и дисбаланс стакана

**Выход:** `tb::order_book::TopOfBook`, `tb::order_book::DepthSummary`

---

### 4. FeatureEngine (Фаза 2)

**Расположение:** `src/features/`

Принимает нормализованные события и вычисляет набор признаков (`FeatureSnapshot`)
для каждого символа. Использует `IndicatorEngine` для технических индикаторов
и `LocalOrderBook` для микроструктурных признаков.

Методы:
- `on_ticker(NormalizedTicker)` — обновляет цену, спред, контекст исполнения
- `on_trade(NormalizedTrade)` — обновляет поток сделок, buy/sell ratio
- `on_candle(NormalizedCandle)` — добавляет свечу в буфер, пересчитывает технику
- `get_snapshot(symbol)` → `FeatureSnapshot`

**Выход:** `tb::features::FeatureSnapshot`

---

### 5. FeatureSnapshot

**Расположение:** `src/features/feature_snapshot.hpp`

Неизменяемый снимок всех признаков для одного символа в момент времени.
Содержит три группы:
- `TechnicalFeatures` — SMA, EMA, RSI, MACD, Bollinger, ATR, ADX, OBV, волатильность
- `MicrostructureFeatures` — спред, дисбаланс стакана, поток сделок, ликвидность
- `ExecutionContextFeatures` — стоимость спреда, проскальзывание, свежесть данных

`is_complete()` возвращает `true` если доступны хотя бы базовые технические
(`sma_valid`) и микроструктурные (`spread_valid`) признаки.

---

### 6. [Pipeline] WorldModelEngine / RegimeEngine / Стратегии

Принимают `FeatureSnapshot` и выполняют:
- **Market Readiness Gate** — проверяет готовность к торговле (HTF валиден, прогрев завершён, RSI норма)
- **WorldModelEngine** — агрегация состояния рынка по всем символам
- **RegimeEngine** — определение рыночного режима (тренд, флэт, волатильность)
- **Стратегии** — генерация торговых сигналов с тренд-фильтрацией
- **HTF Trend Filter** — финальная блокировка сигналов против HTF тренда

---

## Гарантии потока данных

| Компонент | Потокобезопасность | Исключения |
|---|---|---|
| PairScanner | нет (однопоточный, вызывается до pipeline) | не бросает |
| HTF Bootstrap | нет (однопоточный, вызывается при старте) | не бросает |
| BitgetNormalizer | нет (однопоточный) | не бросает |
| LocalOrderBook | мьютекс на чтение/запись | не бросает |
| FeatureEngine | нет (однопоточный) | не бросает |
| IndicatorEngine | stateless, безопасен | не бросает |

## Временны́е метки

Каждое событие несёт три метки (в наносекундах):
- `exchange_ts` — время биржи из payload
- `received_ts` — время получения байт в клиенте
- `processed_ts` — время нормализации

Задержка обработки = `processed_ts - received_ts`.
