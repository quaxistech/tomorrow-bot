# `src/exchange/bitget` — Bitget USDT-M Futures клиент

## Назначение

Низкоуровневая интеграция с Bitget v2 API: REST mix-endpoints, public WebSocket (market data), private WebSocket (fills/positions), HMAC-SHA256 подписи, парсинг моделей.

## Границы ответственности

* `BitgetRestClient` — синхронный HTTP/HTTPS клиент (Boost.Beast + OpenSSL), token bucket rate limiter, clock sync, retry с backoff.
* `BitgetWsClient` — async public WebSocket (PIMPL).
* `BitgetPrivateWsClient` — authenticated private WS для fill/position events.
* `BitgetFuturesOrderSubmitter : IOrderSubmitter` — submit/cancel/query, plan/trigger ордера, set_leverage/margin_mode/hold_mode.
* `BitgetFuturesQueryAdapter : IExchangeQueryService` — для reconciliation.
* `bitget_signing.cpp` — HMAC-SHA256 подпись запросов.
* `bitget_models.hpp` — `RawTicker`, `RawTrade`, `RawOrderBook`, `RawCandle`, `RawWsMessage`.

## Входы / выходы

* REST: `(method, path, query_params|body)` → `RestResponse{status_code, body, success, error_code, error_message}`.
* Public WS: `subscribe(channel)` → callbacks `RawWsMessage`.
* Private WS: `(orders.{instId}, positions.{instId})` → callbacks с парсенным `boost::json::value`.
* Futures Order Submitter: `OrderRecord` → `OrderSubmitResult{exchange_order_id, error_code}`.

## Публичные интерфейсы

* `class BitgetRestClient`:
  * `get/post/del(path, query|body) → RestResponse`.
  * `check_clock_sync() → int64_t offset_ms`, `clock_offset_ms()`.
  * `get_server_time_ms() → int64_t`.
* `class BitgetWsClient`:
  * `start/stop()`, `subscribe(channel)`, `unsubscribe(channel)`, `is_connected()`.
* `class BitgetPrivateWsClient`:
  * аналогично + auth handshake.
* `class BitgetFuturesOrderSubmitter`:
  * `submit_order(OrderRecord)`, `cancel_order(OrderId, Symbol)`,
  * `submit_plan_order/cancel_plan_order` (TP/SL),
  * `query_order_fill_price/detail`,
  * `set_leverage(symbol, leverage, hold_side)`,
  * `set_margin_mode(symbol, "isolated"|"crossed")`,
  * `set_hold_mode(product_type, "single_hold"|"double_hold")`,
  * `set_rules(symbol, ExchangeSymbolRules)`.
* `class BitgetFuturesQueryAdapter`:
  * `get_open_orders/account_balances/open_positions/order_status/trigger_orders`.
* `bitget_signing.hpp`: `make_auth_headers(method, path, body, ts, secret, key, passphrase) → headers`.

## Внутренние компоненты

* `bitget_rest_client.hpp/cpp` — connection pool, token bucket, retry.
* `bitget_ws_client.hpp/cpp` — PIMPL, io_context, reconnect, heartbeat.
* `bitget_private_ws_client.hpp/cpp` — login frame, channel sub, parsed events.
* `bitget_signing.hpp/cpp` — HMAC-SHA256.
* `bitget_models.hpp` — Raw* DTO.
* `bitget_futures_order_submitter.hpp/cpp` — JSON build, leverage cache, max-leverage discovery via Bitget code 40797.
* `bitget_futures_query_adapter.hpp/cpp` — REST → reconciliation DTO.

## Зависимости

* Boost.Asio + Beast (TCP/SSL).
* OpenSSL (HMAC-SHA256, TLS).
* Boost.JSON (парсинг payload).
* `logging`, `clock`, `common`, `execution/order_submitter` (interface).

## Потоки данных

* REST: caller → `BitgetRestClient::get/post/del` → `wait_for_rate_limit(path)` → `execute_once` (создаёт TCP+SSL, подписывает, читает) → retry на transient ошибки → возврат.
* Public WS: io_context thread → on_message → `MarketDataGateway::on_raw_message`.
* Private WS: io_context thread → on_message → `TradingPipeline::on_private_ws_message` → ExecutionEngine.

## Race conditions

* `rate_buckets_` и `conn_pool_` под отдельными mutex'ами.
* Token bucket: refill вычисляется по wall-clock внутри mutex; не lock-free.
* WS callbacks вызываются из io_context thread; пользователь должен синхронизировать (TradingPipeline владеет `pipeline_mutex_`).
* `leverage_cache_`/`max_leverage_cache_` — под `leverage_cache_mutex_`. Запросы set_leverage идемпотентны через cache check.

## Ошибки проектирования

* **D-bg-1 (HIGH).** `BitgetRestClient` использует `std::this_thread::sleep_for` для rate limit ожидания (token bucket с busy-wait). При нехватке токенов hot-path блокируется. Mitigation: вернуть `Result<...>::TooBusy` и предоставить выбор стратегии вызывающему.
* **D-bg-2 (MEDIUM).** Подпись запроса использует body+timestamp; если `clock_offset_ms` не корректируется (`check_clock_sync` вызывается только startup), при NTP-step во время работы запросы получат `INVALID_SIGN_TIMESTAMP`. См. Defect-D2 в корне.
* **D-bg-3 (MEDIUM).** Ошибки Bitget маппятся в `error_code` строкой; внутренний `TbError` не различает типы (`INSUFFICIENT_BALANCE` vs `MIN_NOTIONAL` vs rate limit). Mitigation: расширить `TbError` (см. `common/claude.md`).
* **D-bg-4 (MEDIUM).** Connection pool: `conn_pool_` под mutex, но re-use SSL connections к одному endpoint. После долгого idle сервер закрывает connection — следующий запрос фейлится. Retry covers, но добавляет латентность.
* **D-bg-5 (LOW).** В `bitget_rest_client.hpp` нет защиты от path-injection в `query_params` — caller обязан экранировать вручную.
* **D-bg-6 (LOW).** `set_leverage` идёт через mutex'ный cache, но при race условие гонки между двумя pipeline на одном символе (что не должно происходить из-за `Supervisor::try_lock_symbol`) — поведение неопределено.

## Контракты

### `BitgetRestClient::post(path, json_body)`

* **Pre.** `path` начинается с `/api/v2/...`, `json_body` валидный JSON.
* **Post.** `RestResponse` с `status_code` ∈ {200, 4xx, 5xx, 0}. Если `status_code = 0` — сетевая ошибка. `success = (status_code in [200, 299) ∧ Bitget code = 0)`.
* **Invariant.** Подпись актуальна (`timestamp` отличается от server не более чем на `recvWindow`).
* **Idempotent.** Зависит от endpoint: `/place-order` идемпотентен через `clientOid`.

### `BitgetFuturesOrderSubmitter::submit_order(OrderRecord o)`

* **Pre.**
  * `o.symbol` ∈ `rules_by_symbol_` (вызывался `set_rules`).
  * `o.qty ≥ rules.min_qty`.
  * `o.price * o.qty ≥ rules.min_notional`.
  * `position_side` и `trade_side` валидны для hedge mode.
  * leverage установлен на бирже.
* **Post.**
  * Успех: `result.exchange_order_id != ""`, ордер виден в `/api/v2/mix/order/all`.
  * Неудача: `result.error_code ∈ Bitget`-кодов; cash должен быть released вызывающим.
* **Invariant.** Никогда не отправлять без `clientOid` (идемпотентность).

### `set_leverage(symbol, leverage, hold_side)`

* **Pre.** `leverage ∈ [1, max_leverage_for_symbol]`. `hold_side ∈ {"long","short"}`.
* **Post.** Успех: cache обновлён, биржа применила. Неудача: лог + `false`.
* **Invariant.** Idempotent: повторный вызов с тем же значением → no-op (через cache).

## Производственные риски

* **R-bg-1.** Bitget rate limits: `place-order` 10 req/sec, при превышении 429 → retry → но если retry budget исчерпан, ордер пропадает. Mitigation: глобальный rate budget на уровне `Supervisor`.
* **R-bg-2.** WS reconnect: при reconnect нужно пересубскрибиться. Что происходит с пропущенными `delta` стакана? — `LocalOrderBook` должен запросить snapshot. Reverify integration: `BitgetWsClient::reconnect → MarketDataGateway::on_connection_changed(true) → resubscribe → first message = snapshot`.
* **R-bg-3.** Bitget API изменения (v2 → v3 в будущем): нет version negotiation.
* **R-bg-4.** Подделка endpoint URL не блокируется на уровне `BitgetRestClient` (только в `ProductionGuard`).

## Рекомендации

1. Async REST через Boost.Asio coroutines (без `sleep_for` блокировок).
2. Periodic clock sync (например, каждый час, через `Supervisor` scheduled task).
3. Версионирование API (header) и graceful fallback.
4. TLS pinning по сертификату Bitget.
5. Расширить `TbError` для семантически различимых биржевых ошибок.
6. Тестовый sandbox: использовать Bitget testnet endpoints для smoke-тестов.
7. WS gap detection: метрика `ws_sequence_gaps_total{stream}` для public WS.
