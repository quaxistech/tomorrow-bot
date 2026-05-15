# Модуль normalizer — подробный разбор

Временный рабочий документ.

Дата: 2026-04-10  
Обновлено: 2026-04-10 (production-grade overhaul)

## 1. Назначение модуля

`src/normalizer` — инфраструктурный слой преобразования рыночных сообщений Bitget USDT-M Futures из сырого WebSocket-формата во внутренние типизированные события.

Его задача не в том, чтобы хранить рынок, считать индикаторы или принимать торговые решения. Его задача уже и строже:

1. принять `RawWsMessage` от биржевого WebSocket-клиента;
2. безопасно разобрать JSON-пакет;
3. распознать тип канала (`ticker`, `trade`, `books*`, `candle*`);
4. привести данные к внутренним strong-type структурам;
5. добавить единый metadata-конверт (`EventEnvelope`);
6. отправить нормализованное событие дальше по callback.

В архитектуре это мост между `exchange/bitget` и внутренним market-data pipeline.

## 2. Состав модуля

В каталоге находятся 4 файла:

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | сборка библиотеки `tb_normalizer` |
| `normalized_events.hpp` | внутренние нормализованные структуры и `EventEnvelope` |
| `normalizer.hpp` | интерфейс класса `BitgetNormalizer` |
| `normalizer.cpp` | JSON-разбор и нормализация payload-ов Bitget |

Модуль компактный: публичный API фактически состоит из одного класса `BitgetNormalizer` и одного variant-типа `NormalizedEvent`.

## 3. Сборка и зависимости

`tb_normalizer` собирается как `STATIC` библиотека из одного файла `normalizer.cpp`.

Зависимости:

- `tb_common`
- `tb_logging`
- `tb_clock`
- `tb_exchange_bitget`
- `Boost::json`

Практически это означает следующее:

- strong types (`Symbol`, `Price`, `Quantity`, `Timestamp`, `Side`) приходят из `common/types.hpp`;
- входные raw-модели приходят из `exchange/bitget/bitget_models.hpp`;
- время обработки ставится через инжектированный `IClock`;
- ошибки парсинга и неизвестные каналы пишутся через `ILogger`;
- сам JSON разбирается локально через `boost::json`, без промежуточного DTO-слоя и без codegen.

## 4. Место модуля в реальном runtime-flow

Реальный боевой путь данных выглядит так:

1. `BitgetWsClient` получает сырой frame от биржи;
2. `MarketDataGateway::on_raw_message()` фильтрует heartbeat и subscribe ack;
3. gateway передаёт сообщение в `normalizer_->process_raw_message(msg)`;
4. `BitgetNormalizer` парсит payload и генерирует `NormalizedEvent`;
5. callback normalizer-а возвращает событие в `MarketDataGateway::on_normalized_event()`;
6. gateway маршрутизирует событие в `LocalOrderBook`, `FeatureEngine` и пользовательские callback-и;
7. downstream-модули используют нормализованные данные для вычисления snapshot-ов, индикаторов, trade-flow и т.д.

То есть normalizer находится строго в начале рыночного пайплайна: после transport/WebSocket слоя, но до всех аналитических и торговых подсистем.

## 5. Входной контракт модуля

Публичный вход в модуль один:

```cpp
void process_raw_message(const exchange::bitget::RawWsMessage& msg);
```

`RawWsMessage` содержит:

- `type` — классификация сообщения на уровне WebSocket-клиента;
- `raw_payload` — исходная JSON-строка;
- `received_ns` — время приёма сообщения локальной системой в наносекундах.

Важно: normalizer практически не использует `msg.type` для логики разбора. Основное ветвление идёт по содержимому JSON (`arg.channel`, `arg.instId`, `data`, `action`), а не по enum `WsMsgType`.

Следствие: normalizer опирается на реальную структуру payload-а, а не на предварительную классификацию транспортного слоя.

## 6. Выходной контракт: `NormalizedEvent`

Результат работы normalizer-а — variant:

```cpp
using NormalizedEvent = std::variant<
    NormalizedTicker,
    NormalizedTrade,
    NormalizedOrderBook,
    NormalizedCandle
>;
```

Все четыре типа содержат общий `EventEnvelope`.

### 6.1. `EventEnvelope`

В envelope лежат:

- `exchange_ts`
- `received_ts`
- `processed_ts`
- `symbol`
- `source`
- `sequence_id`

Это делает normalizer не просто парсером чисел, а генератором replay-friendly событий с базовым контекстом происхождения.

### 6.2. Что дают поля envelope

- `exchange_ts` — время события на стороне биржи;
- `received_ts` — время получения локальным процессом;
- `processed_ts` — время, когда normalizer собрал внутреннюю структуру;
- `symbol` — торговый инструмент;
- `source` — строковый источник данных, сейчас всегда `"bitget"`;
- `sequence_id` — локальный монотонный счётчик порядка обработки внутри normalizer-а.

## 7. Нормализованные структуры данных

### 7.1. `NormalizedTicker`

Содержит:

- `last_price`
- `bid`
- `ask`
- `volume_24h`
- `change_24h_pct`
- `spread`
- `spread_bps`

Это уже готовый microstructure-friendly snapshot top-of-book уровня.

### 7.2. `NormalizedTrade`

Содержит:

- `trade_id`
- `price`
- `size`
- `side`
- `is_aggressive`

Именно эти поля потом попадают в `TradeBuffer` и используются downstream для VWAP, buy/sell ratio и aggressive flow.

### 7.3. `NormalizedOrderBook`

Содержит:

- `update_type` (`Snapshot` / `Delta`)
- массивы `bids`
- массивы `asks`
- `sequence`

Это событие используется `LocalOrderBook` для восстановления и поддержки локального стакана.

### 7.4. `NormalizedCandle`

Содержит:

- `interval`
- `open/high/low/close`
- `volume`
- `base_volume`
- `is_closed`

Дальше этот тип идёт в candle buffers и индикаторный стек.

## 8. Класс `BitgetNormalizer`

Конструктор принимает три зависимости:

1. `NormalizedEventCallback callback`
2. `std::shared_ptr<IClock> clock`
3. `std::shared_ptr<ILogger> logger`

Это определяет роль класса:

- callback — куда отправлять готовые события;
- clock — как ставить `processed_ts` и как определять закрытость свечи;
- logger — как фиксировать ошибки JSON и неизвестные каналы.

Дополнительно normalizer хранит:

- `symbols_` — список разрешённых инструментов;
- `symbols_mutex_` — защита списка символов;
- `sequence_` — атомарный счётчик локального порядка событий.

## 9. `set_symbols()` — фильтрация по инструментам

Метод `set_symbols()` сохраняет список `Symbol` под mutex.

При обработке каждого сообщения normalizer:

1. извлекает `instId` из `arg.instId`;
2. если список `symbols_` не пуст, ищет `instId` в нём;
3. если символ не найден — сообщение отбрасывается целиком.

Важно:

- фильтрация выполняется только если `instId` не пустой;
- если `arg.instId` отсутствует, то сообщение не будет отфильтровано на этом этапе.

В реальном runtime это не проблема для штатных Bitget market-data payload-ов, но как поведенческий нюанс модуля это важно.

## 10. Вспомогательные функции в `normalizer.cpp`

В модуле есть 4 локальные helper-функции.

### 10.1. `safe_string(const json::value*)`

Пытается извлечь строку из JSON-значения.

Если:

- указатель пустой;
- значение не строковое;

то возвращает пустую строку.

### 10.2. `safe_string(const json::object&, key)`

Ищет ключ в объекте и делегирует извлечение первой версии `safe_string`.

### 10.3. `to_double()`

Пытается преобразовать строку через `std::stod`.

При ошибке возвращает `0.0`.

### 10.4. `to_int64()`

Пытается преобразовать строку через `std::from_chars`.

При ошибке возвращает `0`.

### 10.5. Поведенческий смысл этих helper-ов

Модуль спроектирован как tolerant parser:

- он предпочитает не падать;
- он предпочитает выпустить частично заполненное событие;
- числовые ошибки чаще конвертируются в нули, чем в исключения.

Это повышает живучесть пайплайна, но создаёт и обратную сторону: silent degradation данных.

## 11. `process_raw_message()` — центральный метод модуля

Это главный runtime-метод normalizer-а.

Его поведение по шагам:

1. если `raw_payload` пустой — сразу `return`;
2. вызывает `boost::json::parse`;
3. при ошибке пишет warning и завершает обработку;
4. убеждается, что корень JSON — объект;
5. достаёт `arg` и убеждается, что это объект;
6. достаёт `channel`, `instId`, `action`;
7. применяет фильтрацию по `symbols_`;
8. достаёт `data` и убеждается, что это непустой массив;
9. по `channel` выбирает конкретную ветку разбора;
10. для каждого элемента `data` создаёт raw DTO, затем нормализованный DTO, затем вызывает callback.

Метод сам по себе не хранит рынок и не буферизует состояние. Он stateless относительно содержимого рынка, кроме счётчика `sequence_` и списка разрешённых символов.

## 12. Обработка ошибок и отказоустойчивость

Если JSON битый, модуль делает следующее:

- пишет `warn` в logger;
- для известной ошибки добавляет `error` и `payload_prefix`;
- не бросает исключение наружу;
- не вызывает callback.

Если канал неизвестный, модуль:

- пишет `debug`-лог `"Неизвестный канал"`;
- не бросает исключение;
- не генерирует событие.

Если структура JSON частично повреждена, но формально разбирается, normalizer чаще молча выпустит событие с нулями или пустыми строками, чем остановит поток.

## 13. Ветка `ticker`

Для канала `ticker` normalizer:

1. берёт `instId` из `arg` или из самого элемента `data`;
2. извлекает `lastPr`, `bidPr`, `askPr`, `baseVolume`, `change24h`, `ts`;
3. собирает `RawTicker`;
4. передаёт его в `parse_ticker()`.

### 13.1. Что делает `parse_ticker()`

Метод заполняет envelope и числовые поля, затем считает:

- `spread = ask - bid`
- `mid = (ask + bid) / 2`
- `spread_bps = spread / mid * 10000`

Спред считается только если `bid > 0` и `ask > 0`.

### 13.2. Практический смысл тикера downstream

Именно `NormalizedTicker` становится основой для:

- `FeatureEngine::on_ticker()`;
- вычисления `FeatureSnapshot`;
- оценки текущего спреда и slippage proxy в execution/risk/strategy слоях.

То есть ошибки в нормализации тикера дальше масштабируются почти на весь торговый пайплайн.

### 13.3. Важный нюанс `change_24h_pct`

Поле называется `change_24h_pct`, но normalizer просто делает:

```cpp
ticker.change_24h_pct = to_double(raw.change_24h);
```

Никакого умножения на `100` нет.

Следствие:

- модуль не нормализует единицу измерения самостоятельно;
- смысл поля полностью зависит от того, в каком формате Bitget прислал `change24h`.

Если upstream присылает `0.02` как долю, то внутри системы останется `0.02`, а не `2.0`.

## 14. Ветка `trade`

Для канала `trade` normalizer:

1. извлекает `tradeId`, `price`, `size`, `side`, `ts`;
2. собирает `RawTrade`;
3. вызывает `parse_trade()`.

### 14.1. Что делает `parse_trade()`

Метод:

- выставляет `trade_id`;
- переводит цену и размер в `Price` и `Quantity`;
- маппит `side` в `tb::Side`;
- ставит `is_aggressive`.

### 14.2. Семантика стороны и агрессии

Логика такая:

- `raw.side == "sell"` -> `Side::Sell`
- всё остальное -> `Side::Buy`

И одновременно:

- `raw.side == "sell"` -> `is_aggressive = true`
- всё остальное -> `false`

Это значит, что текущая трактовка модуля следующая:

- агрессивный поток определяется как sell-side taker flow;
- buy-side трактуется как неагрессивный.

Это очень конкретная семаническая договорённость, и downstream `TradeBuffer::aggressive_flow()` использует её буквально.

### 14.3. Ограничение ветки trade

Если `side` повреждён или пришёл в неожиданном формате, normalizer не отклонит сделку, а классифицирует её как `Buy` и `is_aggressive = false`.

## 15. Ветка `books*`

Для каналов, начинающихся с `books`, модуль:

1. извлекает `action`, `ts`, `seqId`;
2. читает массивы `bids` и `asks`;
3. для каждого уровня берёт первые два элемента `[price, size]`;
4. собирает `RawOrderBook`;
5. вызывает `parse_order_book()`.

### 15.1. Что делает `parse_order_book()`

Метод:

- ставит envelope;
- переводит `action == "snapshot"` в `BookUpdateType::Snapshot`;
- всё остальное трактует как `BookUpdateType::Delta`;
- переносит `raw.sequence` в `book.sequence`;
- переводит уровни в `BookLevel { Price, Quantity }`.

### 15.2. Что происходит downstream

В `MarketDataGateway::on_normalized_event()`:

- snapshot передаётся в `LocalOrderBook::apply_snapshot()`;
- delta передаётся в `LocalOrderBook::apply_delta()`.

Если `apply_delta()` видит разрыв последовательности, gateway:

- логирует warning;
- вызывает `order_book_->request_resync()`;
- ждёт следующий snapshot.

Следовательно, normalizer отвечает не только за форму book update-а, но и за корректную передачу exchange sequence дальше в state machine локального стакана.

### 15.3. Ограничения ветки books

- неизвестный `action` не отвергается, а становится `Delta`;
- уровень с некорректными строками цены/размера превращается в `0.0`;
- extra-поля уровня игнорируются.

## 16. Ветка `candle*`

Для каналов, начинающихся с `candle`, normalizer ожидает, что каждый элемент `data` — массив:

```text
[ts, open, high, low, close, volume, base_volume, is_closed?]
```

### 16.1. Разбор интервала

Интервал берётся из имени канала:

- `candle1m` -> `1m`
- `candle5m` -> `5m`
- `candle15m` -> `15m`
- `candle1h` -> `1h`

Код делает это через `channel.substr(6)`.

### 16.2. Как определяется `is_closed`

Логика трёхступенчатая:

1. если есть 8-й элемент и он равен `"1"`, свеча закрыта;
2. иначе вычисляется `raw.ts + interval_ms <= now_ms`;
3. иначе свеча считается открытой.

`now_ms` берётся не из `system_clock`, а из инжектированного `clock_`, что правильно для replay/backtest окружений.

### 16.3. Поддерживаемые интервалы

В коде явно прописаны только:

- `1m`
- `5m`
- `15m`
- `1h` / `1H`

Все остальные интервалы не отвергаются, но получают дефолт `60_000 ms`.

Это означает, что если upstream или config когда-нибудь начнёт использовать, например, `3m`, `30m` или `4h`, вычисление `is_closed` станет логически неточным.

### 16.4. Что происходит downstream

`FeatureEngine::on_candle()`:

- пишет закрытые свечи через `push()`;
- live-свечи обновляет через `update_last()`.

То есть корректность `is_closed` напрямую влияет на историю свечей, а значит и на все технические индикаторы.

## 17. Семаника времени в модуле

Это один из самых важных аспектов normalizer-а.

### 17.1. Какие времена использует модуль

- `received_ns` приходит в наносекундах;
- `processed_ts = clock_->now()` тоже в наносекундах;
- `raw.ts` из Bitget payload-а трактуется как целое число без масштабирования.

### 17.2. Важное инженерное наблюдение

В `parse_ticker()`, `parse_trade()`, `parse_order_book()`, `parse_candle()` код делает:

```cpp
envelope.exchange_ts = Timestamp{raw.ts};
```

Но `Timestamp` в системе задокументирован как наносекунды от Unix epoch.

При этом в реальных Bitget payload-ах `ts` обычно приходит в миллисекундах.

Следствие:

- `exchange_ts` в envelope, вероятно, хранится в миллисекундах;
- `received_ts` и `processed_ts` точно хранятся в наносекундах;
- внутри одного `EventEnvelope` могут сосуществовать разные единицы времени.

Это не просто локальная деталь.

`TradeBuffer` и `CandleBuffer` дальше сохраняют `trade.envelope.exchange_ts` и `candle.envelope.exchange_ts` как event/open time. Значит потенциальная рассинхронизация единиц времени протекает дальше по пайплайну.

### 17.3. Почему candle closure всё же работает лучше

В вычислении `is_closed` код сознательно переводит локальное время в миллисекунды:

```cpp
auto now_ms = clock_->now().get() / 1'000'000;
```

Поэтому именно проверка закрытия свечи по формуле `raw.ts + interval_ms <= now_ms` выполнена в согласованных единицах.

Но это не исправляет проблему того, что в envelope `exchange_ts` потом всё равно записывается без перевода в наносекунды.

## 18. Последовательности: `sequence_id` vs `sequence`

У normalizer-а есть два разных понятия порядка.

### 18.1. `envelope.sequence_id`

Это локальный атомарный счётчик normalizer-а:

- общий для всех типов событий;
- растёт по мере вызова `parse_*`;
- отражает порядок обработки внутри процесса.

### 18.2. `NormalizedOrderBook.sequence`

Это отдельное поле из биржевого `seqId`, относящееся именно к стакану.

Эти два поля нельзя путать:

- `sequence_id` — локальный processing order;
- `sequence` — exchange order-book sequence.

## 19. Реальная интеграция с `MarketDataGateway`

`MarketDataGateway` — главный runtime consumer normalizer-а.

### 19.1. Создание

В конструкторе gateway:

1. создаёт `BitgetNormalizer`;
2. передаёт ему callback `on_normalized_event()`;
3. передаёт те же `clock_` и `logger_`;
4. сразу вызывает `normalizer_->set_symbols(config_.symbols)`.

### 19.2. Передача сырых сообщений

`on_raw_message()`:

- игнорирует heartbeat;
- игнорирует subscribe ack;
- обновляет `last_message_ts_` только после этой фильтрации;
- увеличивает `raw_message_count_`;
- периодически пишет лог;
- затем вызывает `process_raw_message()`.

Это важный нюанс: свежесть feed-а здесь считается по реальным рыночным сообщениям, а не по служебным frame-ам.

### 19.3. Маршрутизация событий

После normalizer-а gateway делает следующее:

- `NormalizedOrderBook` -> обновляет `LocalOrderBook`;
- `NormalizedTicker` -> обновляет `FeatureEngine` и триггерит `compute_snapshot()`;
- `NormalizedTrade` -> обновляет trade buffer внутри `FeatureEngine` и вызывает внешний trade callback;
- `NormalizedCandle` -> обновляет candle buffers внутри `FeatureEngine`.

## 20. Самый важный runtime-факт

`FeatureSnapshot` строится только на событии `NormalizedTicker`.

Это значит:

- новые trades сами по себе snapshot не триггерят;
- новые candle updates сами по себе snapshot не триггерят;
- order book updates сами по себе snapshot не триггерят;
- cadence downstream feature/pipeline обновлений определяется именно частотой ticker-сообщений.

Для анализа поведения системы это один из ключевых фактов.

## 21. Связь с USDT-M фьючерсным контекстом проекта

По архитектуре бот ориентирован на USDT-M futures.

Это видно не в самом normalizer-е, а в окружающем runtime:

- `MarketDataGateway::GatewayConfig` имеет `inst_type = "USDT-FUTURES"` по умолчанию;
- подписки на биржу формируются именно с этим `instType`;
- downstream логика ордербука, признаков и стратегии рассчитана на фьючерсный поток.

При этом сам `BitgetNormalizer`:

- не проверяет `instType` внутри payload-а;
- не ветвится по spot/futures;
- ориентируется только на форму JSON и названия каналов.

То есть normalizer сейчас не содержит отдельной spot-логики, но и не делает жёсткого futures-only gate внутри себя.

Дополнительный нюанс: существующий integration test для тикера использует payload с `"instType":"SPOT"`, хотя боевой gateway работает с `USDT-FUTURES`.

## 22. Тестовое покрытие

У normalizer-а есть integration test файл `tests/integration/normalizer_test.cpp`.

Сейчас он покрывает только три сценария:

1. битый JSON не вызывает исключений;
2. пустой payload не вызывает исключений;
3. валидный ticker payload не вызывает исключений.

### 22.1. Что тестируется хорошо

- модуль не падает на мусорном JSON;
- модуль не падает на пустом payload;
- basic happy-path для ticker-сообщения существует.

### 22.2. Что не покрыто

Не покрыты напрямую:

- ветка `trade`;
- ветка `books*`;
- ветка `candle*`;
- фильтрация по `symbols_`;
- корректность `spread` и `spread_bps`;
- корректность `side` и `is_aggressive`;
- корректность `seqId` и `BookUpdateType`;
- логика `is_closed`;
- поведение на нестроковых полях;
- вопрос единиц времени в `exchange_ts`.

То есть модуль используется очень рано и очень глубоко в market-data пайплайне, но его тесты пока проверяют в основном “не падает ли он”, а не “правильно ли он нормализует”.

## 23. Важные инженерные наблюдения

### 23.1. Модуль синхронный

`process_raw_message()` вызывает callback прямо в текущем потоке.

Значит:

- normalizer не имеет внутренней очереди;
- нет batching;
- нет async handoff;
- тяжёлый downstream callback будет замедлять поток обработки входящих рыночных сообщений.

### 23.2. Парсер очень толерантен

Это полезно для живучести, но опасно для качества данных.

Некорректная цена, объём, timestamp или side чаще превратятся в `0`/`0.0`/`Buy`, чем будут явно отвергнуты.

### 23.3. `msg.type` почти не участвует в логике

Модуль доверяет JSON-структуре больше, чем предварительной классификации transport слоя.

Это делает normalizer более автономным, но одновременно создаёт дублирование ответственности между `BitgetWsClient` и normalizer-ом.

### 23.4. `instType` в payload сейчас не используется

С точки зрения текущего кода normalizer не отличает futures и spot по содержимому сообщения.

Для данного проекта это компенсируется тем, что боевой gateway подписывается на `USDT-FUTURES`, но сам normalizer не enforce-ит это явно.

### 23.5. Временная семантика `exchange_ts` выглядит неоднородной

Это, вероятно, самое серьёзное наблюдение по модулю.

Если upstream `ts` действительно в миллисекундах, а `Timestamp` в системе трактуется как наносекунды, то normalizer смешивает единицы времени внутри одного envelope.

### 23.6. `change_24h_pct` семанически двусмысленно

Название поля выглядит как “проценты”, но код не делает ни `*100`, ни дополнительной нормализации.

### 23.7. Candle closure зависит от ограниченного набора interval mapping

Для текущих production интервалов (`1m`, `5m`, `1h`) поведение логично.
Для более широкого набора интервалов модуль сейчас не универсален.

### 23.8. Отсутствует внутренняя валидация рыночной физики

Модуль не проверяет, что:

- `ask >= bid`;
- `price > 0`;
- `size >= 0`;
- `high >= low`;
- candle OHLC согласованы между собой;
- order book levels отсортированы или уникальны.

Нормализатор сейчас делает structural normalization, но почти не делает semantic validation.

## 24. Краткий итог

`src/normalizer` — это компактный, важный и очень ранний слой рыночного пайплайна.

Он уже выполняет ключевую работу:

1. принимает сырые Bitget WS payload-ы;
2. безопасно разбирает JSON;
3. преобразует данные в внутренние структуры с strong types;
4. ставит unified envelope;
5. передаёт события в downstream market-data стек.

На практике модуль хорошо встроен в систему:

- используется `MarketDataGateway`;
- правильно отделён от WebSocket transport слоя;
- работает через DI (`clock`, `logger`, callback);
- поддерживает тикеры, сделки, стакан и свечи;
- аккуратно не падает на битых входах.

Но при глубоком разборе видно, что модуль больше похож на надёжный structural parser, чем на полностью завершённый validation layer:

- числовые ошибки чаще молча превращаются в нули;
- `instType` не используется как жёсткий guard;
- `change_24h_pct` не имеет явной нормализации единицы измерения;
- `exchange_ts` очень вероятно смешивает миллисекунды и наносекунды;
- candle interval logic жёстко зашита только под ограниченный набор интервалов;
- покрытие тестами пока узкое.

Именно поэтому normalizer стоит понимать как критичный инфраструктурный адаптер market-data потока, который уже хорошо выполняет базовую нормализацию Bitget -> internal DTO, но требует аккуратного инженерного внимания при любом аудите качества данных, replay-семантики и latency-sensitive фьючерсного runtime.