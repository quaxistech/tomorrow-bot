# `src/normalizer` — Нормализатор Bitget WS

## Назначение

Парсинг сырых WebSocket сообщений Bitget USDT-M Futures и преобразование в строго типизированные `NormalizedEvent`. Единственный модуль, знающий о Bitget JSON layout (вне `exchange/bitget`).

## Границы ответственности

* Парсинг 4 типов сообщений: ticker, trade, orderbook (snapshot+delta), candle.
* Фильтрация non-futures сообщений (по `instType`).
* Извлечение и нормализация timestamps (ms → ns).
* Назначение монотонной последовательности событий.
* Подсчёт rejected events для observability.

## Границы (что НЕ делает)

* Не валидирует sequence continuity (это `LocalOrderBook`).
* Не накапливает state (только парсит inbound payload).
* Не отвечает за reconnect/resubscribe.

## Входы / выходы

* Вход: `RawWsMessage{channel, payload, received_ts}` от `BitgetWsClient`.
* Выход: callback `NormalizedEventCallback(NormalizedEvent)`.

## Публичные интерфейсы

* `class BitgetNormalizer`:
  * `BitgetNormalizer(callback, clock, logger)`.
  * `void process_raw_message(const RawWsMessage&)`.
  * `void set_symbols(std::vector<Symbol>)` — фильтр по символам.
  * `uint64_t rejected_count() const` (atomic).
* `NormalizedEvent` — variant{Ticker, Trade, OrderBook, Candle}.
* `NormalizedEventCallback = std::function<void(NormalizedEvent)>`.

## Внутренние компоненты

* `normalizer.hpp/cpp` — main parser.
* `normalized_events.hpp` — DTO `NormalizedTicker/Trade/OrderBook/Candle`, `EventEnvelope`, `BookLevel`.

## Зависимости

* `exchange/bitget/bitget_models.hpp` — `RawTicker`, `RawTrade`, `RawOrderBook`, `RawCandle`.
* `clock/IClock`, `logging/ILogger`.
* Boost.JSON для парсинга.

## Потоки данных

```
RawWsMessage
  → BitgetNormalizer::process_raw_message
       → boost::json::parse(payload)
       → topic match ("ticker"/"trade"/"books"/"candle"+interval)
       → check instType ∈ {"USDT-FUTURES"}
       → check symbol ∈ symbols_ (если фильтр установлен)
       → parse_<type>(raw, received_ns) → optional<NormalizedX>
       → fill_envelope(symbol, exchange_ts_ms, received_ns, sequence_++)
       → callback_(NormalizedEvent{...})
```

## Race conditions

* `symbols_` под `symbols_mutex_` — read многими потоками, write через `set_symbols`.
* `sequence_` атомарный.
* `rejected_count_` атомарный.

## Ошибки проектирования

* **D-norm-1 (MEDIUM).** Parsing payload для каждого сообщения создаёт новый `boost::json::value` (heap allocation). На пиковом потоке 200+ msg/s это hot-path GC-pressure. Mitigation: pool allocator (см. § 7 в корне).
* **D-norm-2 (LOW).** `parse_*` методы возвращают `optional<>`; на ошибку парсинга только инкрементируется `rejected_count_`, но конкретное событие не логируется (нет sample log). Затрудняет отладку при рассинхронизации с Bitget.
* **D-norm-3 (LOW).** `set_symbols` не очищает state нормализатора (если уже шли события для удалённого символа — они дойдут до callback, фильтрация только до). Условие гонки с `symbols_mutex_` корректно, но семантика не очевидна.

## Контракты

### `process_raw_message(msg)`

* **Pre.** `msg.payload` UTF-8 строка.
* **Post.**
  * Успех (валидный futures-event для известного символа): callback вызван с `NormalizedEvent`. `EventEnvelope.received_ns ≥ msg.received_ts` (используется clock в момент парсинга).
  * Ошибка: `rejected_count_` инкрементирован.
* **Invariant.** `sequence_` строго возрастает между всеми callback вызовами (в рамках одного нормализатора).

## Производственные риски

* **R-norm-1.** Bitget может изменить структуру payload (несовместимые версии API). Mitigation: контракт-тесты на real-world fixtures.
* **R-norm-2.** Утечка памяти при повторных аллокациях `string` для symbol/instId — решается через `string_view` где возможно.

## Рекомендации

1. Профайлинг under load: 1000 events/s × 8 символов. При >2% CPU overhead — pool allocator.
2. Sample logging: каждое 100-е rejection — info-лог с диагностикой.
3. Контракт-тест fixture: записанный Bitget WS трафик за 1 минуту, replay-тест.
4. Метрика `normalizer_rejected_total{reason}`.
