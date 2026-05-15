# `src/pipeline` — Торговый конвейер

## Назначение

Главный orchestrator системы. **Per-symbol** класс `TradingPipeline` владеет всеми runtime engines (market data, analytics, ML, leverage, risk, execution, telemetry) и связывает их в единый цикл обработки тика. Также управляет watchdog, exit orchestrator, hedge pair manager, dual-leg manager, market reaction engine, latency tracker.

## Границы ответственности

* Создание и запуск всех downstream engines per symbol (~30 объектов).
* Hot-path: `on_feature_snapshot(snapshot)` — последовательное исполнение всех 28 gates от feature до order.
* HTF (1h) trend tracking + adaptive update.
* Trailing stop + breakeven + partial TP.
* Funding rate update.
* Periodic tasks: reconciliation (60 s), balance sync (5 min), watchdog (10 s), HTF (60 min), incident check (10 s), snapshot (30 s).
* Hedge recovery + dual-leg pair management.
* Persistence (snapshots + position events).
* Trade history rolling window (для Kelly + portfolio_allocator).
* Diag counters per gate (kDiagLogLimit = 10).

## Структура каталога

* `trading_pipeline.hpp/cpp` (~580 строк header, ~3570 строк implementation).
* `pipeline_tick_context.hpp` — контекст одного тика.
* `pipeline_stage_result.hpp` — результаты стадий.
* `pipeline_latency_tracker.hpp/cpp` — P50/P95/P99.
* `order_watchdog.hpp/cpp` — мониторинг жизненного цикла ордеров.
* `exit_orchestrator.hpp/cpp` + `exit_types.hpp` — единый владелец exit-решений.
* `hedge_pair_manager.hpp/cpp` — state machine хедж-позиции.
* `dual_leg_manager.hpp/cpp` — coordinated long+short pair management.
* `market_reaction_engine.hpp/cpp` — reactivity to market state changes.
* `pair_economics.hpp` — экономика парных позиций.
* `pair_execution_coordinator.hpp` — координация исполнения пары.
* `pair_lifecycle_engine.hpp` — lifecycle парной позиции.

## Публичные интерфейсы

* `class TradingPipeline`:
  * Конструктор `(AppConfig, secret_provider, logger, clock, metrics, symbol = "", shared_portfolio = nullptr)`.
  * `start() → bool`, `stop()`.
  * `symbol() → const Symbol&`.
  * `set_symbol_precision(qty_prec, price_prec)`.
  * `set_exchange_rules(ExchangeSymbolRules)`.
  * `set_num_pipelines(int)`.
  * `is_connected()`, `has_open_position()`.
  * `is_idle(threshold_ns)`, `last_activity_time_ns()`.

## Внутренние компоненты

См. полный список fields в `trading_pipeline.hpp`. Ключевые:

* Market data: `IndicatorEngine`, `FeatureEngine`, `LocalOrderBook`, `MarketDataGateway`.
* Analytics: `IWorldModelEngine`, `IRegimeEngine`, `IUncertaintyEngine`.
* Strategy: `StrategyRegistry`, `IStrategyAllocator`, `IDecisionAggregationEngine`.
* Sizing: `IPortfolioEngine` (shared), `IPortfolioAllocator`, `IExecutionAlphaEngine`, `IOpportunityCostEngine`, `IRiskEngine`, `LeverageEngine`.
* Execution: `ExecutionEngine`, `SmartTwapExecutor`.
* ML: `BayesianAdapter`, `EntropyFilter`, `MicrostructureFingerprinter`, `LiquidationCascadeDetector`, `CorrelationMonitor`, `ThompsonSampler`.
* Persistence: `PersistenceLayer`.
* Telemetry: `ResearchTelemetry`, `IncidentDetector`, `ObservabilityPanels`.
* Periodic: `PipelineLatencyTracker`, `OrderWatchdog`, `PositionExitOrchestrator`, `MarketReactionEngine`, `ReconciliationEngine`.
* Exchange: `BitgetRestClient`, `BitgetPrivateWsClient`, `BitgetFuturesOrderSubmitter`, `BitgetFuturesQueryAdapter`.
* Hedge: `HedgePairManager`, `DualLegManager`.

## Зависимости

Практически все модули системы.

## Потоки данных

См. § 4.1 в корневом `claude.md`. Основной цикл — `on_feature_snapshot(snapshot)` под `pipeline_mutex_`.

Отдельный cold-path: `on_private_ws_message(channel, data)` — события исполнения от биржи.

## Race conditions

* Главный `pipeline_mutex_` сериализует hot-path и event handlers.
* Атомарные: `running_`, `last_activity_ns_`, `tick_count_`.
* `std::jthread ref_price_thread_` — фоновый запрос BTC/ETH.
* Public WS thread + Private WS thread + jthread + main scheduling — потенциально 4+ потока обращаются к engines.

## Ошибки проектирования

* **D-pl-1 (HIGH).** Класс ~580 строк state, ~50 полей — god-object. Watchdog/exit/hedge правильно вынесены, но pipeline всё ещё owner всех ML, telemetry, leverage, HTF state, trailing state, daily reset, persistence. Нарушает SRP. См. **Defect-D6 в корне**.
* **D-pl-2 (HIGH).** Все REST-задачи (reconciliation, balance sync, HTF update) внутри hot-path тика — блокирует WS обработку. См. **Defect-D2 в корне**.
* **D-pl-3 (HIGH).** `pipeline_mutex_` единый для public WS + private WS + scheduler — bottleneck. См. **R-6 в корне**.
* **D-pl-4 (MEDIUM).** 14+ diag-block счётчиков с лимитом kDiagLogLimit=10 — после 10 блокировок per gate перестают логироваться без сброса. Может скрывать продолжающиеся проблемы.
* **D-pl-5 (MEDIUM).** Trailing-stop логика встроена в pipeline (поля `highest_price_since_entry_`, `lowest_price_since_entry_`, `current_stop_level_`) — должно быть в `ExitOrchestrator` или `TrailingStopManager`.
* **D-pl-6 (MEDIUM).** Hedge-state дублируется: `hedge_active_/hedge_position_side_/hedge_size_/...` поля + `HedgePairManager` объект. Двойная истина.
* **D-pl-7 (LOW).** `kPhantomGracePeriodNs = 5s` — magic number; должен быть в config.
* **D-pl-8 (LOW).** Constants для periodic intervals (kReconciliationIntervalNs, kSnapshotIntervalNs, ...) хардкоден; конфигурируемость limited.

## Контракты

### `TradingPipeline::start() → bool`

* **Pre.** Все DI зависимости валидные. Symbol установлен (или дефолт BTCUSDT).
* **Post.**
  * `true`: все engines запущены, market_data_gateway connected (или начат retry), private WS зарегистрирован, periodic tasks scheduled.
  * `false`: ошибка инициализации, log.
* **Invariant.** После `start()` → `stop()` → симметричная остановка без leak.

### `on_feature_snapshot(snapshot)`

* **Pre.** `snapshot.symbol = symbol_`. Часто вызывается из ws thread.
* **Post.** Полный цикл pipeline пройден. `tick_count_++`. `last_activity_ns_` обновлён при non-veto.

### `has_open_position() → bool`

* **Pre.** Никаких.
* **Post.** True если portfolio имеет хотя бы одну позицию для symbol_.
* **Invariant.** Используется supervisor для безопасной отмены pipeline (нельзя убивать с открытой позицией).
* **EDGE-16 anti-pyramiding (2026-05-14):** Перед `execution_engine_->execute()` для `intent.trade_side == TradeSide::Open` дополнительно проверяется `has_open_position()`. Если true — Open intent отклоняется с логом `BLOCK Open: already have local position (anti-pyramiding EDGE-16)`. Защита от pyramiding: на каждой scan rotation (10min) создаётся новый `DualLegManager`, который теряет state о ранее открытой позиции. Без guard'а strategy продолжает генерировать Open intents → размер позиции растёт каскадно → liquidation. Live evidence: run31 SAGA -$4.11 при position 1537 (5× normal).
* **Scope:** проверка per-pipeline = per-symbol. Бот свободно открывает позиции на ДРУГИХ symbol'ах (top_n=3 параллельных pipelines). Block применяется только если новая позиция на том же symbol, что и existing.

## Производственные риски

* **R-pl-1.** Все ранее описанные риски архитектуры (D-pl-2, D-pl-3) сходятся в pipeline.
* **R-pl-2.** State drift: при race private WS event и hot-path tick — locked-state обновлений может рассогласоваться.
* **R-pl-3.** При длительном сетевом сбое pipeline_mutex_ зависает на REST → весь pipeline без heartbeat.

## Рекомендации

1. **R-pl-Big.** Декомпозиция:
   * `class HtfTrendTracker` (HTF state + update).
   * `class TradingHistoryStats` (rolling trade window + win_rate).
   * `class FundingRateTracker`.
   * `class TrailingStopManager`.
   * `class SnapshotPersister` (periodic snapshots).
   * `TradingPipeline` ≈ 200-300 строк orchestrator.
2. **R-pl-Big.** Разделение mutex'ов: `market_state_mutex_` (public WS) и `execution_state_mutex_` (private WS, ордера).
3. **R-pl-Big.** Все REST в фоновые потоки с MPSC очередью результатов.
4. Конфиг для всех periodic intervals.
5. Reset diag counters по period (например, каждые 1000 тиков).
6. Тест: TSAN на synthetic feed, проверка отсутствия race.

## EDGE-29-EXIT-FIX (2026-05-15, run48 → run49)

`exit_orchestrator.cpp::check_continuation_value_exit` (строки 736-820):

* `shallow_loss_floor` расширен с `-round_trip_fee × 1.5` → `× 3.0`.
* `bearish gate threshold` поднят с `cont_val > -0.15` → `> -0.22`.

**Why**: на микро-аккаунте $5-10 notional × 0.06% taker × 2 = 0.012 USDT round-trip = ~0.12% на цене. Market noise сопоставим с fee burden. Run48: 2 consecutive HYPEUSDT trades закрылись continuation_exit при PnL ~-0.011 USDT (cont_val=-0.117 и -0.159) — exit lock-in fees + lock-in adverse noise без шанса recovery. Старый floor 1.5× fee = -0.0098 USDT слишком тесен.

**Invariant**: continuation_exit — это **alpha exit**, не hard-risk kill switch. Должен срабатывать только когда:
1. cont_val ниже dynamic threshold (-0.08 ± regime/uncertainty adj), И
2. PnL вне economic dead zone (-3× fee … +2.5× fee × quick_profit_multiplier), И
3. ≥2 bearish confirms ИЛИ cont_val < -0.22.

Hard stops (`atr_stop_multiplier × ATR`), trailing stops, EDGE-17/27 fast adverse — отдельные защиты от реального adverse move.
