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

* `trading_pipeline.hpp/cpp` (~600 строк header, ~3700 строк implementation).
* `pipeline_tick_context.hpp` — контекст одного тика.
* `pipeline_stage_result.hpp` — результаты стадий.
* `pipeline_latency_tracker.hpp/cpp` — P50/P95/P99.
* `order_watchdog.hpp/cpp` — мониторинг жизненного цикла ордеров.
* `exit_orchestrator.hpp/cpp` + `exit_types.hpp` — emergency-tier exits (после edge-31 Phase 5: только hard_capital_stop + toxic_flow + stale_data_exit + update_trailing).
* `pre_trade_gates.hpp/cpp` — **edge-31 Phase 2:** `FreshnessGate` + `NetRRGate` перед entry execute().
* `protective_bracket_manager.hpp/cpp` — **edge-31 Phase 3:** owner TP/SL bracket lifecycle (verify, fallback standalone plan, update_sl для trailing, release, recover). Orphan cleanup на старте.
* `periodic_trailing_sl.hpp/cpp` — **run87:** monotonic trailing SL через ProtectiveBracketManager.update_sl(). Chandelier Exit с adaptive multiplier (supertrend/CVD/cascade-aware). Clamp на breakeven+fees (никогда не trigger в loss-zone). Activation min profit 20 bps (run95).
* `stagnant_position_detector.hpp/cpp` — **run90:** detect "застывшие" позиции (range<12bps/180s, age>15min, hold<30min hard max). Force exit только в loss-zone>30bps (run95 не фиксируем мелкие проседания).
* `hedge_pair_manager.hpp/cpp` — state machine хедж-позиции.
* `dual_leg_manager.hpp/cpp` — coordinated long+short pair management.
* `market_reaction_engine.hpp/cpp` — reactivity to market state changes. **run94:** MarketStateVector forwards 10 advanced indicators (supertrend, AVWAP, CVD, OI, liq cascade, spoof, funding).
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

## edge-31 — Two-layer exit/TPSL refactor (2026-05-16)

**Идея.** Защита позиции живёт на бирже (exchange-attached TP/SL + standalone plan-ордера), pipeline'у остаются только emergency safety nets и adaptive trailing-push. Все алгоритмические exits, которые перекрывались с TP/SL, удалены.

### Phase 1 — Foundation (TPSL plumbing)
* `TradeIntent` обогащён: `take_profit_price`, `stop_loss_price`, `signal_snapshot_ts_ns`, `signal_snapshot_mid`, `signal_snapshot_spread_bps`, `signal_snapshot_depth_usd`.
* `Setup` (strategy state) обогащён `tp_reference`.
* `ScalpStrategyConfig` per-setup ATR multipliers (default R:R = 1.5).
* `ExecutionEngine::create_order_record` копирует `attached_tp_sl` из intent (только для TradeSide::Open). Bitget submitter уже отправляет `presetStopSurplusPrice` / `presetStopLossPrice`.

### Phase 2 — Pre-trade gates (`pre_trade_gates.cpp`)
* `FreshnessGate`: max_signal_age_ms=500, max_adverse_price_drift_bps=8, max_spread_widen_pct=50, min_depth_remain_pct=50.
* `NetRRGate`: min_net_rr=0.5 после fees+slippage+funding. Считает gross_rr = TP_dist / SL_dist, вычитает `taker_fee_bps × 2 + slippage_bps × 2 [+ funding_bps if include_funding_cost]`.
* Запускаются ТОЛЬКО для `TradeSide::Open` сразу перед `execution_engine_->execute()`. Closes/reduces пропускаются.
* `PreTradeGatesConfig` → `AppConfig.pre_trade_gates` → production.yaml.

### Phase 3 — ProtectiveBracketManager (`protective_bracket_manager.cpp`)
* `on_position_opened` — после fill регистрирует bracket state (tp_price, sl_price, source = PresetAttached).
* `verify_brackets(symbol, ps)` — по истечении grace_ms запрашивает open plan-orders на бирже; находит matching loss_plan / profit_plan / pos_tpsl, сохраняет их order_ids.
* После `max_verify_attempts` если plan не обнаружен → fallback `submit_plan_order(StopMarket)` стандартным путём через `BitgetFuturesOrderSubmitter`.
* `update_sl(symbol, ps, new_sl_price)` — для trailing (Phase 4): ставит новый standalone plan, затем cancels старого (атомарность "always protected").
* `release(symbol, ps)` — на закрытии позиции отменяет known plan-ордера (anti-orphan).
* `recover_from_exchange(open_positions)` — на старте восстанавливает state.
* Периодический tick verify в `run_periodic_tasks` (интервал 1500ms).

### Phase 4 — Trailing → bracket SL push
* `update_trailing_stop` после вычисления `current_stop_level_` проверяет, что новый SL улучшает текущий bracket SL (для long: выше; для short: ниже), и `abs_move_pct ≥ 0.05%`. Тогда `bracket_manager_->update_sl()`.
* `last_pushed_trailing_sl_` хранит последнее значение для anti-spam.
* Локальные close-on-trailing-breach триггеры (`check_trailing_stop` в orchestrator) удалены.

### Phase 5 — Exit stack simplification
* `PositionExitOrchestrator::evaluate()` теперь содержит ТОЛЬКО:
  1. `check_fixed_capital_stop` — последняя защита на случай отказа exchange SL.
  2. `check_toxic_flow` — VPIN toxic + meaningful loss.
  3. `check_stale_data_exit` — feed not fresh + adverse condition.
* Удалены: `check_price_stop`, `check_trailing_stop`, `check_partial_tp`, `check_quick_profit`, `check_structural_failure`, `check_liquidity_deterioration`, `check_market_regime_exit`, `check_funding_carry_exit`, `compute_continuation_state`, `check_continuation_value_exit`, inline EDGE-30 force_tp_high и fast_adverse_tiered.
* `update_trailing` сохранён — он вычисляет новый SL для Phase 4 push'а.

### Phase 6 — TradeJournal (`src/telemetry/trade_journal.cpp`)
* Row-per-trade structured log с `signal_age_ms`, `mfe_bps`, `mae_bps`, `giveback_bps`, `exit_layer`.
* Lifecycle hooks: `on_entry_filled` после fill, `on_tick` на каждом update_trailing_stop, `on_exit_filled` при position close.
