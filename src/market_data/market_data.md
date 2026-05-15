# Подробный разбор модуля market_data

Временный аналитический документ.

Источник разбора: текущая реализация `src/market_data` и её прямых runtime-зависимостей на момент 2026-04-06.
Документ описывает фактическое поведение кода, а не только намерение архитектуры.

## 1. Что такое `market_data` в этой системе

Модуль `market_data` в проекте очень компактный по числу файлов, но находится на критической границе системы.

Он не занимается:

- торговыми решениями;
- стратегиями;
- управлением риском;
- исполнением ордеров.

Он делает другое: завершает путь рыночных данных от сырых WebSocket-сообщений до внутреннего `FeatureSnapshot`, который потом потребляет `TradingPipeline`.

Если коротко, текущая роль модуля такая:

```text
Bitget WebSocket
  -> BitgetWsClient
  -> BitgetNormalizer
  -> LocalOrderBook + FeatureEngine
  -> FeatureSnapshot callback
  -> TradingPipeline
```

То есть `market_data` это не самостоятельный аналитический движок, а orchestration-слой между transport, normalization, stateful order book и feature extraction.

## 2. Состав модуля

В папке `src/market_data` сейчас три файла:

| Файл | Назначение |
|---|---|
| `src/market_data/CMakeLists.txt` | собирает библиотеку `tb_market_data` |
| `src/market_data/market_data_gateway.hpp` | интерфейс и состояние `MarketDataGateway` |
| `src/market_data/market_data_gateway.cpp` | реализация жизненного цикла, подписок и маршрутизации событий |

Но по факту модуль работает только в связке с четырьмя прямыми зависимостями:

1. `exchange/bitget/bitget_ws_client.*`
2. `normalizer/normalizer.*`
3. `order_book/order_book.*`
4. `features/feature_engine.*`

Без них `MarketDataGateway` был бы лишь пустой оболочкой.

## 3. Что собирается линковщиком

`tb_market_data` линкуется с:

- `tb_common`
- `tb_logging`
- `tb_metrics`
- `tb_clock`
- `tb_exchange_bitget`
- `tb_normalizer`
- `tb_order_book`
- `tb_features`

Это хорошо отражает реальную ответственность модуля: он не вычисляет бизнес-логику сам, а соединяет transport и state/feature layer.

## 4. Главный объект: `MarketDataGateway`

### 4.1. Что он делает

`MarketDataGateway` это фасад верхнего уровня для потока рыночных данных.

Его основные задачи:

1. поднять WebSocket-клиент Bitget;
2. оформить подписки на нужные каналы;
3. принимать сырые сообщения;
4. прогнать их через нормализатор;
5. разнести нормализованные события по двум stateful контурам:
   - локальный стакан `LocalOrderBook`;
   - агрегатор признаков `FeatureEngine`;
6. по готовности вычислить `FeatureSnapshot`;
7. отправить его дальше по callback-у.

### 4.2. Что он не делает

`MarketDataGateway` не парсит бизнес-смысл рынка глубоко.

Он не считает:

- RSI, EMA, ATR, MACD;
- imbalance, aggressive flow, liquidity ratio;
- торговые сигналы.

Он только направляет поток и определяет, когда нужно попытаться собрать snapshot.

## 5. Интерфейс `GatewayConfig`

Конфиг шлюза очень простой:

```cpp
struct GatewayConfig {
    exchange::bitget::WsClientConfig ws_config;
    std::vector<tb::Symbol> symbols;
    std::vector<std::string> intervals{"1m", "5m", "1h"};
    std::string inst_type{"SPOT"};
    bool subscribe_tickers{true};
    bool subscribe_trades{true};
    bool subscribe_order_book{true};
    bool subscribe_candles{true};
};
```

Практический смысл полей:

- `ws_config` задаёт сетевое поведение WebSocket-клиента;
- `symbols` определяет список инструментов;
- `intervals` задаёт candle-каналы;
- `inst_type` переключает Bitget namespace, например `SPOT` или `USDT-FUTURES`;
- флаги подписок включают и выключают типы каналов.

Важно: по умолчанию `inst_type = "SPOT"`, но в боевом `TradingPipeline` gateway создаётся уже с `inst_type = "USDT-FUTURES"`.

## 6. Полный runtime-поток данных

Ниже фактическая последовательность для одного входящего WebSocket frame.

### 6.1. Шаг 1. Bitget отправляет JSON-frame

Примеры каналов, которые ожидает код:

- `ticker`
- `trade`
- `books15`
- `candle1m`
- `candle5m`
- `candle1h`

### 6.2. Шаг 2. `BitgetWsClient` читает frame

`BitgetWsClient` работает асинхронно на отдельном `io_context` потоке.

На `on_read()` он:

1. превращает network buffer в строку;
2. создаёт `RawWsMessage`;
3. классифицирует тип сообщения:
   - `Heartbeat`
   - `Subscribe`
   - `Error`
   - `Ticker`
   - `Trade`
   - `OrderBook`
   - `Candle`
   - `Unknown`
4. сохраняет `received_ns` как текущее время `system_clock` в наносекундах;
5. вызывает callback `on_message`.

Это важно: слой WebSocket не знает ни про стакан, ни про индикаторы, ни про pipeline.

### 6.3. Шаг 3. `MarketDataGateway::on_raw_message()`

Gateway получает `RawWsMessage` и сразу делает несколько вещей:

1. обновляет `last_message_ts_ = clock_->now()`;
2. отбрасывает heartbeat-сообщения;
3. отбрасывает подтверждения подписки;
4. увеличивает `raw_message_count_`;
5. пишет периодический лог каждые 500 сообщений;
6. передаёт frame в `normalizer_->process_raw_message(msg)`.

Тонкий момент: `last_message_ts_` обновляется до фильтрации heartbeat и subscribe ack. Значит метод `is_feed_fresh()` отслеживает скорее “жив ли сокет и идут ли какие-то frames”, а не строго “приходят ли настоящие рыночные обновления”.

### 6.4. Шаг 4. `BitgetNormalizer::process_raw_message()`

Нормализатор выполняет структурный разбор JSON и превращает биржевой формат в внутренние события.

Он:

1. парсит JSON через `boost::json`;
2. достаёт `arg.channel`, `arg.instId`, `action`;
3. фильтрует сообщения по списку символов `symbols_`;
4. достаёт массив `data`;
5. по типу канала вызывает нужный parser:
   - `parse_ticker()`
   - `parse_trade()`
   - `parse_order_book()`
   - `parse_candle()`
6. отправляет наружу `NormalizedEvent`.

### 6.5. Что такое `EventEnvelope`

Все нормализованные события получают единый metadata-конверт:

- `exchange_ts`
- `received_ts`
- `processed_ts`
- `symbol`
- `source`
- `sequence_id`

Это хороший дизайн: downstream-код получает не только цену и объём, но и временной и replay-friendly контекст.

### 6.6. Как нормализуются разные типы сообщений

#### `NormalizedTicker`

Нормализатор извлекает:

- last price;
- bid;
- ask;
- 24h volume;
- 24h change;
- spread;
- spread в bps.

#### `NormalizedTrade`

Нормализатор извлекает:

- trade id;
- price;
- size;
- side;
- флаг `is_aggressive`.

Текущая семантика такая:

- `raw.side == "sell"` считается агрессивным sell pressure;
- `raw.side != "sell"` трактуется как buy.

#### `NormalizedOrderBook`

Нормализатор превращает `bids/asks` в массивы `BookLevel` и определяет:

- snapshot это или delta;
- sequence номер.

#### `NormalizedCandle`

Нормализатор извлекает OHLCV и пытается определить `is_closed`:

1. либо по явному флагу в 8-м элементе массива;
2. либо по условию `ts + interval <= now`.

### 6.7. Шаг 5. `MarketDataGateway::on_normalized_event()`

Это центральная точка маршрутизации нормализованных событий.

Через `std::visit` gateway делает следующее.

#### Если событие `NormalizedOrderBook`

- при `Snapshot` вызывает `order_book_->apply_snapshot(ev)`;
- иначе вызывает `order_book_->apply_delta(ev)`.

#### Если событие `NormalizedTicker`

1. вызывает `feature_engine_->on_ticker(ev)`;
2. если есть callback `on_snapshot_`, пытается построить snapshot:
   - `feature_engine_->compute_snapshot(symbol, *order_book_)`;
3. если snapshot получен, передаёт его вверх.

#### Если событие `NormalizedTrade`

- вызывает `feature_engine_->on_trade(ev)`.

#### Если событие `NormalizedCandle`

- вызывает `feature_engine_->on_candle(ev)`.

## 7. Самый важный факт о поведении модуля

`FeatureSnapshot` сейчас вычисляется только на событии `ticker`.

Это означает:

- сделки обновляют trade buffer, но не вызывают snapshot немедленно;
- book delta обновляет локальный стакан, но не вызывает snapshot немедленно;
- новые свечи обновляют candle buffer, но не вызывают snapshot немедленно.

Следствие:

реальный cadence входа в pipeline определяется частотой `ticker`-сообщений, а не всеми событиями рынка.

Это архитектурно очень важное ограничение текущей реализации.

## 8. Жизненный цикл `MarketDataGateway`

### 8.1. Конструктор

Конструктор делает две вещи:

1. создаёт `BitgetNormalizer` и связывает его callback-ом с `on_normalized_event()`;
2. создаёт `BitgetWsClient` и связывает его:
   - callback-ом сообщений с `on_raw_message()`;
   - callback-ом состояния соединения с `on_connection_changed()`.

Дополнительно normalizer сразу получает список символов для фильтрации.

### 8.2. `start()`

`start()`:

1. выставляет `running_ = true`;
2. запускает WebSocket-клиент;
3. вызывает `subscribe_to_channels()`.

Порядок важен:

- подписки отправляются сразу после `start()`;
- если соединение ещё не поднято, `BitgetWsClient::subscribe()` просто складывает их в внутренний список `subscriptions`;
- когда handshake завершится, клиент сам переотправит все накопленные подписки.

Это хороший и практичный дизайн: gateway может подписаться сразу, не дожидаясь handshaked-состояния вручную.

### 8.3. `subscribe_to_channels()`

Для каждого символа gateway собирает JSON-объект Bitget v2 формата:

```json
{"instType":"USDT-FUTURES","channel":"ticker","instId":"BTCUSDT"}
```

И подписывается на:

- `ticker`
- `trade`
- `books15`
- `candle{interval}` для каждого интервала из `config_.intervals`

По умолчанию интервалов три: `1m`, `5m`, `1h`.

### 8.4. `stop()`

`stop()` защищён от повторного вызова через CAS на `running_`.

Он:

1. переводит `running_` в `false`;
2. вызывает `ws_client_->stop()`.

Внутри WebSocket-клиента это ведёт к:

- отмене reconnect timer;
- отмене heartbeat timer;
- попытке graceful close;
- `ioc.stop()`;
- `join()` io-thread.

### 8.5. Деструктор

Если gateway ещё запущен, деструктор вызывает `stop()`.

Это безопасно и соответствует RAII-модели владения transport-слоем.

## 9. Как устроен `BitgetWsClient`

Хотя это формально внешний модуль, без него поведение `market_data` нельзя понять.

### 9.1. Что умеет клиент

`BitgetWsClient` умеет:

- асинхронный DNS resolve;
- TCP connect;
- TLS handshake;
- WebSocket handshake;
- heartbeat `ping`;
- reconnect с экспоненциальным backoff;
- сериализацию write-операций через внутреннюю очередь;
- хранение подписок для повторной отправки после reconnect.

### 9.2. Heartbeat

По умолчанию heartbeat идёт каждые 25 секунд.

Если write падает, клиент вызывает `handle_disconnect()` и запускает reconnect.

### 9.3. Reconnect

Backoff строится так:

- старт с 1000 ms;
- затем удвоение;
- максимум 30000 ms.

Это неплохой production-friendly механизм восстановления соединения.

### 9.4. Подписки и очередь отправки

Клиент хранит все channel-spec строки в `subscriptions` и переотправляет их при новом подключении.

Кроме того, он serializes async write-ы через `write_queue_`, потому что `Boost.Beast` не допускает параллельные `async_write` на одном сокете.

Это сильная сторона реализации.

## 10. Как устроен `LocalOrderBook` в контуре market data

`LocalOrderBook` хранит локальную копию L2 стакана.

### 10.1. Что он умеет

- принять snapshot;
- принять delta;
- проверить непрерывность sequence;
- вычислить top-of-book;
- вычислить depth summary;
- держать состояние качества стакана.

### 10.2. Поведение snapshot

`apply_snapshot()`:

- очищает bid/ask maps;
- применяет уровни;
- обновляет `last_sequence_`;
- обновляет `last_updated_`;
- ставит `BookQuality::Valid`.

### 10.3. Поведение delta

`apply_delta()`:

1. если book ещё `Uninitialized` или уже `Desynced`, сразу возвращает `false`;
2. иначе проверяет непрерывность `delta.sequence == last_sequence + 1`;
3. если sequence broken, пишет warning, переводит стакан в `Desynced` и возвращает `false`;
4. если всё хорошо, применяет уровни и остаётся `Valid`.

### 10.4. Что даёт downstream-коду

`LocalOrderBook` отдаёт:

- `TopOfBook`:
  - best bid;
  - best ask;
  - spread;
  - spread_bps;
  - mid price;
  - updated_at;
  - quality.
- `DepthSummary`:
  - bid/ask depth 5;
  - bid/ask depth 10;
  - imbalance 5;
  - imbalance 10;
  - weighted mid.

### 10.5. Ограничение текущей интеграции

Хотя `apply_delta()` может вернуть `false` и стакан может перейти в `Desynced`, `MarketDataGateway` сейчас никак на это не реагирует.

То есть gateway:

- не вызывает `request_resync()`;
- не инициирует повторную подписку;
- не сообщает наверх о деградации канала отдельно.

Это один из заметных operational gaps текущей реализации.

## 11. Как устроен `FeatureEngine` в контуре market data

`FeatureEngine` это уже не часть `market_data` формально, но именно он превращает поток сырья в осмысленный snapshot, который gateway отправляет вверх.

### 11.1. Внутреннее состояние

`FeatureEngine` хранит под mutex:

- `candle_buffers_` по символу;
- `trade_buffers_` по символу;
- `last_tickers_` по символу.

### 11.2. Что происходит на событиях

#### `on_candle()`

- если свеча закрыта, она пушится в candle buffer;
- если свеча ещё живая, обновляется последняя.

#### `on_trade()`

- сделка добавляется в trade buffer.

#### `on_ticker()`

- последний ticker для символа просто заменяется новым.

### 11.3. Когда `compute_snapshot()` вернёт `nullopt`

Snapshot не строится, если:

1. для символа нет достаточного числа свечей;
2. нет последнего тикера.

По текущей логике readiness требует минимум:

```text
max(sma_period, ema_slow_period, macd_slow) + 1
```

При дефолтных настройках это 51 свеча.

### 11.4. Что входит в `FeatureSnapshot`

В snapshot попадают три группы данных:

1. `TechnicalFeatures`
2. `MicrostructureFeatures`
3. `ExecutionContextFeatures`

Плюс поля:

- `symbol`
- `computed_at`
- `market_data_age_ns`
- `last_price`
- `mid_price`
- `book_quality`

### 11.5. Technical features

Считаются:

- SMA;
- EMA fast/slow;
- RSI;
- MACD;
- Bollinger Bands;
- ATR;
- ADX;
- OBV;
- volatility 5/20;
- momentum 5/20.

### 11.6. Microstructure features

Считаются:

- spread;
- spread_bps;
- mid price;
- weighted mid;
- book imbalance;
- bid/ask depth notionals;
- liquidity ratio;
- buy/sell ratio;
- aggressive flow;
- trade VWAP;
- book instability.

### 11.7. Execution context

Считаются:

- spread cost;
- immediate liquidity;
- estimated slippage;
- market open flag;
- feed freshness.

## 12. Граница ответственности между модулями

### `market_data`

Отвечает за:

- запуск и остановку data-feed;
- подписки;
- приём сырых сообщений;
- маршрутизацию нормализованных событий;
- вызов snapshot callback.

### `normalizer`

Отвечает за:

- безопасный JSON parse;
- адаптацию схемы Bitget к внутренним типам;
- упаковку envelope metadata.

### `order_book`

Отвечает за:

- локальное состояние стакана;
- sequence integrity;
- top-of-book/depth views.

### `features`

Отвечает за:

- хранение candle/trade/ticker state;
- расчёт признаков;
- readiness logic;
- сборку `FeatureSnapshot`.

### `pipeline`

Отвечает уже за:

- дальнейший анализ;
- принятие торгового решения;
- риск;
- исполнение.

Это важная граница: `market_data` завершает data plane и не лезет в trade plane.

## 13. Ключевые state-поля `MarketDataGateway`

| Поле | Роль |
|---|---|
| `config_` | что и как подписывать |
| `feature_engine_` | вычисление snapshot |
| `order_book_` | state стакана |
| `logger_`, `metrics_`, `clock_` | инфраструктура |
| `on_snapshot_` | callback вверх, обычно в pipeline |
| `ws_client_` | transport layer |
| `normalizer_` | schema translation |
| `running_` | флаг жизненного цикла |
| `last_message_ts_` | gateway-level freshness |
| `raw_message_count_` | диагностический счётчик |

## 14. Freshness semantics: здесь есть два разных смысла “свежести”

Это один из самых важных нюансов модуля.

### 14.1. Gateway-level freshness

`MarketDataGateway::is_feed_fresh()` считает feed свежим, если:

```text
clock_->now() - last_message_ts_ < 1s
```

Но `last_message_ts_` обновляется на любом raw message, включая heartbeat.

Следствие:

этот метод измеряет скорее живость соединения, чем свежесть именно торговых данных.

### 14.2. Snapshot-level freshness

`FeatureEngine::compute_execution_context()` отдельно вычисляет:

```text
now_ns - ticker.envelope.received_ts < feed_freshness_ns
```

Это уже свежесть последнего тикера, а не любого сокет-сообщения.

### 14.3. Ещё один тонкий момент: два источника времени

Есть потенциально опасная смесь clock domain-ов:

- `RawWsMessage.received_ns` ставится через `system_clock` внутри `BitgetWsClient`;
- `processed_ts`, `computed_at`, `last_message_ts_` ставятся через инжектированный `IClock`.

В production это, вероятно, совпадает по смыслу.
Но в replay/backtest/test clock режимах такая смесь может давать некорректные age/freshness значения.

## 15. Самое важное ограничение текущей реализации

### 15.1. Свечи разных таймфреймов сейчас смешиваются в одном буфере

Это самый существенный архитектурный риск, который видно по коду.

Почему:

1. `MarketDataGateway` по умолчанию подписывается сразу на `1m`, `5m`, `1h` candle channels.
2. `BitgetNormalizer` корректно проставляет `candle.interval`.
3. Но `FeatureEngine::on_candle()` кладёт свечу в `candle_buffers_[symbol]` только по символу.
4. `CandleBuffer` вообще не хранит таймфрейм как ключ сегментации.

Следствие:

в одном candle buffer могут оказаться вперемешку `1m`, `5m` и `1h` свечи.

А затем `compute_technical()` считает SMA/EMA/ATR/ADX/MACD так, как будто это один однородный ряд.

Это не просто косметический дефект, а логически некорректное поведение для технических индикаторов.

Практический эффект:

- readiness может сработать раньше, чем должен;
- индикаторы могут считаться на смешанном таймряде;
- snapshot может выглядеть “валидным”, но быть статистически испорченным.

Это главный скрытый риск текущего модуля.

## 16. Другие важные риски и скрытое поведение

### 16.1. Snapshot генерируется только на `ticker`

Если рынок активно меняет стакан и поток сделок, но ticker update приходит реже, pipeline будет получать обновления с задержкой относительно microstructure state.

### 16.2. Нет реакции на desync стакана

`LocalOrderBook` умеет обнаружить разрыв sequence и перейти в `Desynced`, но gateway не инициирует resync flow.

### 16.3. `BookQuality::Stale` и `BookQuality::Resyncing` фактически не используются

Типы качества объявлены, но текущая реализация реально использует в основном:

- `Uninitialized`
- `Valid`
- `Desynced`

Это означает, что state model стакана богаче, чем фактическая runtime-логика вокруг него.

### 16.4. `start()` не защищён от повторного вызова

В отличие от `stop()`, метод `start()` не использует CAS или другой guard.

Теоретически повторный вызов может:

- повторно запустить `ws_client_->start()`;
- накапливать duplicate subscriptions;
- породить некорректный lifecycle.

По текущему дизайну gateway лучше считать одноразовым объектом: создали, запустили, остановили, уничтожили.

### 16.5. `BitgetWsClient` хранит subscriptions без deduplication

Если один и тот же channel-spec подписать несколько раз, вектор подписок не отфильтрует дубликат.

### 16.6. `MarketDataGateway::is_feed_fresh()` сейчас не используется

По поиску по репозиторию этот метод нигде больше не вызывается.

Значит реальный runtime опирается на freshness внутри `FeatureSnapshot`, а не на gateway-level freshness API.

## 17. Сильные стороны реализации

### 17.1. Хорошее разделение слоёв

Transport, normalization, order book state и feature extraction разнесены по модулям достаточно чисто.

### 17.2. Production-friendly WebSocket клиент

Есть:

- reconnect;
- heartbeat;
- TLS;
- serial write queue;
- хранение подписок на reconnect.

### 17.3. Нормализатор устойчив к плохому JSON

Код не падает на битом payload, а логирует warning и продолжает работу.

### 17.4. Order book отслеживает sequence integrity

Это очень важная защита от тихой порчи локального стакана.

### 17.5. Snapshot строится как `optional`

Gateway не проталкивает наверх “полупустой” snapshot, если не хватает данных.

## 18. Слабые стороны реализации

### 18.1. Нет полноценного end-to-end orchestration вокруг деградации стакана

Sequence break виден, но реакция системы на него неполная.

### 18.2. Смешение таймфреймов свечей

Это главный архитектурный дефект текущей реализации.

### 18.3. Freshness semantics разорваны на два разных механизма

Один уровень следит за socket activity, другой за age тикера, причём на разных clock domain-ах.

### 18.4. Snapshot cadence жёстко привязан к ticker channel

Это упрощает дизайн, но может занижать чувствительность к быстрым trade/book изменениям.

### 18.5. Тестовое покрытие неполное

По коду найдены:

- integration tests для `BitgetNormalizer`;
- unit tests для `LocalOrderBook`.

Но прямых тестов для:

- `MarketDataGateway`;
- `FeatureEngine`;
- полного пути `WS message -> snapshot callback`

по поиску не обнаружено.

## 19. Что реально получает `TradingPipeline` от этого модуля

С точки зрения pipeline, `market_data` даёт следующее обещание:

1. у меня есть подписанный живой feed;
2. я накапливаю состояние ticker/trade/candle/order book;
3. когда приходит очередной ticker и данных хватает, я дам `FeatureSnapshot`.

Внутри `TradingPipeline` это используется через callback:

```text
MarketDataGateway(..., [this](FeatureSnapshot snap) {
    on_feature_snapshot(std::move(snap));
})
```

То есть для pipeline модуль `market_data` это “внешний поставщик готового рыночного снимка”, а не набор отдельных потоков событий.

## 20. Краткий итог

Текущий модуль `market_data` реализует тонкий, но критический фасад между биржевым WebSocket-транспортом и торговым pipeline.

Его фактическая работа выглядит так:

1. асинхронно получает frames от Bitget;
2. классифицирует их и нормализует;
3. обновляет локальный стакан и feature buffers;
4. на каждом ticker пытается собрать единый `FeatureSnapshot`;
5. передаёт snapshot наверх в pipeline.

Сильные стороны модуля:

- чёткое разделение обязанностей;
- устойчивый reconnecting WebSocket client;
- нормализованный внутренний формат событий;
- sequence-aware local order book.

Главные проблемы текущей версии:

- snapshots рождаются только на ticker events;
- gateway не завершает resync flow после desync стакана;
- freshness semantics неоднозначны;
- самое серьёзное: свечи разных таймфреймов складываются в один буфер и затем используются для расчёта индикаторов как единый ряд.

Если описать модуль одной фразой: это boundary-layer рыночных данных, который правильно соединяет transport и downstream analytics, но пока содержит критически важное скрытое допущение о candle data, способное искажать всю последующую аналитику.