# `src/market_data` — Шлюз рыночных данных

## Назначение

Верхний слой рыночных данных. Связывает `BitgetWsClient` (Bitget public WS) с `BitgetNormalizer` → `LocalOrderBook` + `FeatureEngine`. Выход: `FeatureSnapshot` для downstream pipeline.

## Границы ответственности

* Конфигурация подписок (tickers, trades, books, candles на нескольких интервалах).
* Lifecycle (start/stop), reconnect (через `BitgetWsClient::Impl`).
* Фильтрация на уровне instType=USDT-FUTURES.
* Детекция «свежести» feed'а (`is_feed_fresh`).

## Входы / выходы

* Вход: WebSocket-фреймы Bitget → callback `on_raw_message`.
* Выход: `FeatureSnapshotCallback` (вызывается после обновления feature engine), `TradeCallback` (для downstream consumers).

## Публичные интерфейсы

* `class MarketDataGateway`:
  * Конструктор: `(GatewayConfig, FeatureEngine, LocalOrderBook, logger, metrics, clock, on_snapshot, on_trade)`.
  * `start()`, `stop()`.
  * `is_connected() → bool`.
  * `is_feed_fresh() → bool` (по `last_message_ts_`).
* `struct GatewayConfig` — `WsClientConfig`, символы, интервалы (`{"1m","5m","1h"}`), флаги подписок.
* `using FeatureSnapshotCallback = std::function<void(FeatureSnapshot)>`.
* `using TradeCallback = std::function<void(price, volume, is_buy)>`.

## Внутренние компоненты

* `market_data_gateway.hpp/cpp`.

## Зависимости

* `exchange/bitget/bitget_ws_client.hpp` — public WS.
* `normalizer/normalizer.hpp` — `BitgetNormalizer`.
* `order_book/order_book.hpp` — `LocalOrderBook`.
* `features/feature_engine.hpp`.
* `logging`, `metrics`, `clock`.

## Потоки данных

```
ws thread → on_raw_message(msg)
  → atomic<int64_t> last_message_ts_ = now
  → atomic<uint64_t> raw_message_count_++
  → BitgetNormalizer::process_raw_message(msg)
       → on_normalized_event(NormalizedEvent)
            → если OrderBook: order_book_->apply_snapshot/delta
            → если Ticker: feature_engine_->on_ticker
            → если Trade: feature_engine_->on_trade + on_trade_(...) callback
            → если Candle: feature_engine_->on_candle
  → когда есть данные для FeatureSnapshot → feature_engine_->compute_snapshot
       → on_snapshot_(snapshot) — callback в pipeline
```

## Race conditions

* `running_` атомарный.
* `last_message_ts_` атомарный.
* `raw_message_count_` атомарный.
* `feature_engine_`/`order_book_` имеют свои mutex'ы.
* Callback `on_snapshot_`/`on_trade_` вызывается из ws-thread → caller (pipeline) обязан синхронизироваться.

## Ошибки проектирования

* **D-md-1 (MEDIUM).** `compute_snapshot` вызывается на каждом event (или на каждом тике?) — это дорогой операция (полный пересчёт всех индикаторов из buffer). См. **D-feat-1 в `features`**.
* **D-md-2 (MEDIUM).** Нет dedicated thread для downstream pipeline. WS-thread напрямую вызывает callback → pipeline обработка (`on_feature_snapshot`) держит ws-thread → дальнейшие WS frames в очередь Boost.Asio. При медленной обработке pipeline backlog растёт.
* **D-md-3 (LOW).** `is_feed_fresh()` не реализует threshold из конфига (предположительно константа). Mitigation: вынести `feed_freshness_threshold_ns` в `GatewayConfig`.
* **D-md-4 (LOW).** `subscribe_to_channels` использует жёсткий список интервалов из конфига; при изменении в конфиге требуется restart pipeline.

## Контракты

### `MarketDataGateway::start()`

* **Pre.** Все DI-зависимости не nullptr. `running_ = false`.
* **Post.** WS соединение установлено (или начат retry); `running_ = true`. После первого `Open` callback'а — подписки отправлены.
* **Invariant.** `start()` идемпотентен: повторный вызов — no-op.

### `MarketDataGateway::stop()`

* **Pre.** Никаких.
* **Post.** WS закрыт, callbacks больше не вызываются. `running_ = false`.

## Производственные риски

* **R-md-1.** Backlog при медленной обработке pipeline → memory growth. Mitigation: bounded queue + drop policy + alert.
* **R-md-2.** Reconnect не сохраняет sequence в стакане → `LocalOrderBook` обязан получить snapshot первым (контролируется Bitget protocol).
* **R-md-3.** Параллельные подписки на 5+ символов × 4 канала (ticker, trade, book, candles×3 интервала) ≈ 20+ подписок per WS connection. Bitget лимиты: проверить.

## Рекомендации

1. Dedicated thread для pipeline → MPSC очередь между WS thread и pipeline.
2. Bounded queue с drop-oldest или drop-newest policy + alert metric.
3. Снизить нагрузку: вызывать `compute_snapshot` только на завершение тика, а не на каждый event.
4. Конфигурируемый `feed_freshness_threshold_ns`.
5. Тест: simulate WS reconnect, проверка восстановления stакана.
