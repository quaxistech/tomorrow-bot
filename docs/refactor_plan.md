# Refactor Plan — Tomorrow Bot

**Статус (обновлено 2026-05-16, edge-31 exit/TPSL refactor):**

* Сборка clean: `g++-14 -std=c++23`, все таргеты.
* Тесты: **659 / 659 passed.**
* Все D1-D14 + scalping refactor (2026-05-16 первая итерация) сохраняются: реализация описана ниже как design-документ.

## edge-31 — Exit / Position management refactor (2026-05-16)

Полная переработка exit/protection layer на двухслойную архитектуру:

* **Layer 1 — Exchange-attached protection.** Entry order несёт `presetStopLossPrice` /
  `presetStopSurplusPrice` (Phase 1). Bitget автоматически создаёт pos-attached TP/SL
  плэн-ордера. `ProtectiveBracketManager` (Phase 3) после fill verify'ит их на бирже,
  fallback'ит standalone plan если preset не сработал, восстанавливает state при рестарте.
* **Layer 2 — Adaptive trailing.** `update_trailing_stop` вычисляет новый SL уровень и
  через `bracket_manager_->update_sl()` (Phase 4) пушит cancel+replace plan-ордер.
  Локальный close-on-trailing-breach trigger в orchestrator удалён.
* **Pre-trade gates (Phase 2).** Перед `execution_engine_->execute()` стоят
  `FreshnessGate` (max_signal_age_ms, adverse_price_drift_bps, spread_widen_pct,
  depth_remain_pct) и `NetRRGate` (net R:R после fees+slippage+funding).
* **Exit stack simplification (Phase 5).** Из `PositionExitOrchestrator` удалены 10
  overlapping exits: `check_price_stop`, `check_trailing_stop`, `check_partial_tp`,
  `check_quick_profit`, `check_structural_failure`, `check_liquidity_deterioration`,
  `check_market_regime_exit`, `check_funding_carry_exit`, `compute_continuation_state`,
  `check_continuation_value_exit`, плюс inline EDGE-30 force_tp_high и
  fast_adverse_tiered. Остались только 3 emergency-tier: `check_fixed_capital_stop`,
  `check_toxic_flow`, `check_stale_data_exit`.
* **TradeJournal (Phase 6).** Row-per-trade телеметрия с `signal_age_ms`, `mfe_bps`,
  `mae_bps`, `giveback_bps`, `exit_layer` (HardCapital / ExchangeTP / ExchangeSL /
  TrailingSL / ToxicFlow / StaleData / SignalDriven / Manual).

### Новые модули

* `src/pipeline/pre_trade_gates.{hpp,cpp}` — `FreshnessGate`, `NetRRGate`.
* `src/pipeline/protective_bracket_manager.{hpp,cpp}` — owner bracket lifecycle.
* `src/telemetry/trade_journal.{hpp,cpp}` — row-per-trade journal.

### Структурные правки

* `TradeIntent` (`strategy_types.hpp`): добавлены `take_profit_price`, `stop_loss_price`,
  `signal_snapshot_ts_ns`, `signal_snapshot_mid`, `signal_snapshot_spread_bps`,
  `signal_snapshot_depth_usd`.
* `Setup` (`setup_models.hpp`): добавлено `tp_reference`.
* `ScalpStrategyConfig`: per-setup ATR multipliers (`atr_{stop,target}_mult_{momentum,retest,pullback,rejection}`).
* `ExecutionEngine::create_order_record` (`execution_engine_new.cpp`): копирует
  `attached_tp_sl` из intent (только для `TradeSide::Open`).
* `BitgetFuturesOrderSubmitter::submit_plan_order` / `cancel_plan_order` — `virtual`
  (для unit-test override).
* `BitgetFuturesQueryAdapter::get_open_plan_orders` — новый метод (endpoint
  `/api/v2/mix/order/orders-plan-pending`).
* `PreTradeGatesConfig` секция в `AppConfig` + production.yaml + config_loader/validator.

### Тесты

* `tests/unit/pipeline/pre_trade_gates_test.cpp` — 18 тестов (FreshnessGate, NetRRGate).
* `tests/unit/pipeline/protective_bracket_manager_test.cpp` — 10 тестов (lifecycle).
* `tests/unit/telemetry/trade_journal_test.cpp` — 5 тестов.
* `tests/unit/execution/execution_test.cpp` — 3 новых TPSL-теста.
* `tests/unit/pipeline/exit_orchestrator_test.cpp` — переписан под simplified path (7 тестов).
* Всего: **+36 новых тестов, ~10 obsolete тестов удалены**.



**Scalping refactor (2026-05-16) — поверх D1-D14:**

* Удалены модули `src/ml/{regime_ensemble,meta_label,calibration}` (~1387 LOC + тесты): zero внешних ссылок.
* Удалён `src/strategy_allocator/` (~373 LOC + тесты): degenerate при N=1 active strategy. Типы `AllocationResult`/`StrategyAllocation` перенесены в `src/decision/strategy_allocation.hpp`. Вес считается inline в `trading_pipeline.cpp:2911+`.
* `src/world_model/` переписан (1998→722 LOC, −64%): 9-state адаптивная машина с гистерезисом, transition matrix 9×9, feedback-EMA, multi-driver scoring заменена pure-stateless классификатором. `world_model_history.hpp` удалён.
* `src/uncertainty/` переписан (1134→539 LOC, −52%): 9-мерная композитная модель → 4 hard-сигнала + 3 soft.
* `src/decision/` surgical cleanup: BUY/SELL conflict resolution, ensemble bonus, `compute_regime_threshold_factor`, `compute_regime_dominance_threshold` удалены. Conviction threshold переписан в bounded severity-max + cost-headroom формулу с hard cap 0.80.
* `src/leverage/leverage_engine.cpp::uncertainty_multiplier` нейтрализован для Low/Moderate/High (был double-counted с `uncertainty.size_multiplier`).
* Maker-first execution path: `intent.urgency` 0.9→0.30; `ExecutionPlanner` маппит `PostOnly`/`Passive` → `PostOnlyLimit`; `FillProcessor` использует maker/taker fee rate по `order_type`. `production.yaml` пере-tюнен (urgency_aggressive_threshold 0.50→0.80, postonly_spread_threshold_bps 8→12).
* `AdvancedDecisionConfig`/`DecisionConfig`/`UncertaintyConfig`/`WorldModelConfig` очищены от dormant полей; `config_loader`/`config_validator`/`production.yaml` синхронизированы.
* 5 устаревших EDGE-30-эпохи exit_orchestrator-тестов обновлены под текущую калибровку.

Объёмы: ~−3500 LOC src/ + ~−750 LOC устаревших тестов = ~−4250 LOC чистого сокращения.

Документ ниже фиксирует **дизайны и фактические результаты** D1-D14.

## Сводка реализаций

Поглощены и закрыты в одном проходе:

* **D1** — 5 orphan-модулей (`adversarial_defense`, `alpha_decay`, `drift`, `self_diagnosis`, `validation`) и их orphan-тесты удалены. Pipeline уже реализует adversarial detection локально (см. `trading_pipeline.cpp` строки 3849-3921 — построено на `WorldState`, `spread_bps`, `book_instability`, VPIN, `aggressive_flow`, `ml.fingerprint_edge`); orphan-модуль не давал runtime-функциональности.
* **D8** — `interruptible_sleep` + ранний `signal handler` в `main.cpp` (граничность опроса 200 мс).
* **D12** — micro-account scaling вынесен в `ScannerConfig` (`micro_account_capital_threshold_usdt`, `micro_min_orderbook_depth_usdt`, `micro_min_open_interest_usdt`).
* **D13** — пустые `audit.md` / `plan.md` удалены.
* **D14** — `Testing/`, `CTestTestfile.cmake`, `DartConfiguration.tcl` добавлены в `.gitignore`.

Аннулированы как ложные срабатывания исходного аудита (после повторной верификации):

* **D7** — `LocalOrderBook::emit_events` уже копирует subscribers под mutex и вызывает callbacks без удержания (`order_book.cpp:352-361`, BUG-ML-01 fix).
* **D9** — `ExecutionEngine::execute()` корректно: `is_duplicate` → `register_order` → `transition` → `try_reserve_margin` → submit. Все под `execute_mutex_`, на failure submit/exception идёт `release_cash`. Race нет.
* **D11** — `BitgetRestClient::wait_for_rate_limit` отпускает `rate_mutex_` перед `sleep_for` (строки 145-147). Token-bucket корректен.

**ВСЕ остальные дефекты ВНЕДРЕНЫ В КОД** в рамках staged-refactor 2026-05-07:

* **D2** (CRITICAL) — ✅ внедрён `RestWorkerPool` (jthread + bounded MPSC + drop-oldest политика); 3 периодические REST-задачи (`balance_sync`, `reconciliation`, `funding_rate`) переведены на async submit. См. файлы [`rest_worker_pool.hpp`](../src/pipeline/rest_worker_pool.hpp), [`rest_worker_pool.cpp`](../src/pipeline/rest_worker_pool.cpp), и `run_periodic_tasks` в `trading_pipeline.cpp`.
* **D3** (HIGH) — ✅ внедрён `KillSwitchProvider` callback в `IRiskEngine`. На каждом `evaluate()` риск-движок синхронизирует local state из authoritative источника (Supervisor). Race window закрыт.
* **D4** (HIGH) — ✅ внедрена fail-fast верификация всех 4 setup-вызовов в `TradingPipeline::start()`. При отказе → `start()` returns false → Supervisor блокирует pipeline.
* **D5** (MEDIUM) — ✅ замена самописного flat YAML на `yaml-cpp 0.8`. `parse_yaml_flat` теперь через `YAML::Load` + рекурсивный flatten. Sequences автоматически в comma-separated string. Sentinel `__yaml_parse_error` для fail-fast.
* **D6** (HIGH) — ✅ извлечены 3 класса: [`TradingHistoryStats`](../src/pipeline/trading_history_stats.hpp), [`FundingRateTracker`](../src/pipeline/funding_rate_tracker.hpp), [`HtfTrendState`](../src/pipeline/htf_trend_state.hpp). Pipeline хранит как member-объекты; 16 HTF-полей + 4 funding-поля + Kelly stats inline-удалены из `TradingPipeline::*_`.
* **D10** (LOW) — задокументировано в `src/clock/claude.md`.

Раздел ниже сохранён как **архитектурный дизайн-документ** с подробностями реализации, для справки и продолжающейся работы (split mutex, дополнительная декомпозиция, TSAN integration).

---

## D2 — Async REST за пределами hot-path

### Текущий дефект

`TradingPipeline::on_feature_snapshot` исполняет под `pipeline_mutex_`:

* `run_continuous_reconciliation` → REST `get_open_orders` + `get_open_positions` + `get_account_balances` (3+ запроса, ~300 мс под нагрузкой).
* `sync_balance_from_exchange` → REST.
* `maybe_update_htf` → REST candles.
* `update_funding_rate` → REST.

При сетевом jitter тики WS накапливаются в Boost.Asio очереди, нарушают `sequence_continuity` стакана.

### Дизайн

#### Архитектура

```
┌─────────────────┐     ┌─────────────────┐
│ Hot-path tick   │     │ REST Worker     │
│ (WS thread)     │     │ (jthread)       │
└────────┬────────┘     └────────┬────────┘
         │ tick                  │
         ▼                       │
   schedule_task                 │
   (SPSC queue)                  │
         │                       │
         └─────► RestTaskQueue ──┘
                       │
                       ▼
               result published to
               atomic<RestResult> snapshots
                       │
   ┌───────────────────┴───────────────────┐
   │ Hot-path on next tick reads snapshots │
   │ via load_acquire (no blocking)        │
   └───────────────────────────────────────┘
```

#### Компоненты

1. **`RestWorkerPool`** (новый класс в `pipeline/rest_worker_pool.hpp/cpp`):
   ```cpp
   class RestWorkerPool {
   public:
       RestWorkerPool(size_t worker_count, ILogger&, IMetricsRegistry&);
       ~RestWorkerPool();   // jthread.request_stop в дструкторе
       
       // Submit неблокирующий — task в queue
       template<typename F>
       void submit(std::string name, F&& fn);
       
   private:
       std::vector<std::jthread> workers_;
       boost::lockfree::spsc_queue<Task, capacity<256>> tasks_;
       std::condition_variable cv_;  // wakeup
   };
   ```

2. **`PeriodicTaskScheduler`** (отдельная сущность):
   ```cpp
   class PeriodicTaskScheduler {
   public:
       void schedule(std::string name, std::chrono::nanoseconds interval, std::function<void()> task);
       void start(RestWorkerPool& pool);
       void stop();
   };
   ```

3. **Snapshot publisher pattern**:
   ```cpp
   struct ReconciliationSnapshotAtomic {
       std::shared_ptr<const ReconciliationResult> latest;  // store/load_acquire
   };
   ```

#### Миграция (по этапам)

| Этап | Действие | Тестирование |
|------|----------|--------------|
| 1 | Внедрить `RestWorkerPool`, добавить unit-тесты на queue + worker lifecycle | `tests/unit/pipeline/rest_worker_pool_test.cpp` |
| 2 | Перевести `update_reference_prices` (уже jthread'овский!) на новый pool | regression: existing test |
| 3 | Перевести `sync_balance_from_exchange` | mock submitter, проверка периодичности |
| 4 | Перевести `maybe_update_htf` | symbol-specific test |
| 5 | Перевести `run_continuous_reconciliation` (CRITICAL — задействует state mutation в portfolio при mismatch) | TSAN test |
| 6 | Перевести `update_funding_rate`, `incident_check` | metrics test |
| 7 | Удалить `last_*_reconciliation_ns_`, `last_balance_sync_ns_` поля | code review |

#### Инварианты

* **Inv-12 (reconciliation freshness).** Сохраняется: `RestWorkerPool::submit` возвращает control немедленно, scheduler фиксирует timestamp последнего успешного прогона.
* **Inv-C1.** При async published snapshot — обязателен `std::atomic<std::shared_ptr<...>>` или RCU. Никаких read-write race.
* **Inv-Inv-3 (idempotent fills).** Не затронут — реconciliation fill-events идут через ExecutionEngine, не через REST snapshot.

#### Риски

* TSAN-тестирование обязательно: race между snapshot publish и concurrent read.
* Reconciliation auto-resolve вносит mutation в portfolio — выполнить через `Supervisor`-coordinated callback, чтобы не разбегались с execution path.
* Backpressure: queue full → drop oldest non-critical task, метрика `rest_pool_queue_dropped_total`.

#### Estimated effort

~7-10 PR, 3-4 недели работы senior engineer. Каждый этап — отдельный PR с тестами и canary deploy на staging.

---

## D3 — Унификация kill-switch RiskEngine ↔ Supervisor

### Текущий дефект

* `Supervisor` хранит `std::atomic<bool> kill_switch_active_`, broadcast'ит в listeners.
* `ProductionRiskEngine` имеет собственный `std::atomic<bool> kill_switch_active_`, синхронизируется через listener.
* Между `Supervisor::activate_global_kill_switch(reason)` и `RiskEngine::kill_switch_active_.store(true)` — окно гонки. В этом окне `RiskEngine::evaluate` может одобрить ордер.

### Дизайн

#### Принципиальная развилка

* **Вариант A (выбран):** RiskEngine читает state у Supervisor. Single source of truth.
* Вариант B: оставить листенер, добавить sequence-numbers — отвергнуто (сложнее, race остаётся).

#### Реализация

1. Изменить `IRiskEngine`:
   ```cpp
   class IRiskEngine {
   public:
       virtual void set_kill_switch_provider(
           std::function<std::pair<bool, std::string>()> provider) = 0;
       // ...прочие методы
   };
   ```

2. В `ProductionRiskEngine::evaluate()` первая проверка читает provider:
   ```cpp
   const auto [active, reason] = kill_switch_provider_();
   if (active) {
       decision.action = RiskAction::EmergencyHalt;
       decision.summary = "kill_switch: " + reason;
       return decision;
   }
   ```

3. В `pipeline` при создании risk_engine:
   ```cpp
   risk_engine_->set_kill_switch_provider([sup = supervisor_handle_]() {
       return std::make_pair(sup->is_kill_switch_active(), sup->kill_switch_reason());
   });
   ```

4. Удалить `ProductionRiskEngine::kill_switch_active_` поле и `activate/deactivate_kill_switch` методы из interface.

5. `Supervisor::register_kill_switch_listener` остаётся для НЕ-state нотификаций (cancel-all, alert).

#### Контракт (обновлённый)

`IRiskEngine::evaluate(...)`:

* **Pre.** `kill_switch_provider_` установлен.
* **Post.** Если `provider() = {true, _}` → `decision.allowed = false ∧ decision.action = EmergencyHalt` без оценки прочих чеков.
* **Invariant.** **Inv-8** в корневом `claude.md`: kill switch domination — теперь в одной атомарной точке (Supervisor).

#### Миграция

| Этап | Действие |
|------|----------|
| 1 | Расширить `IRiskEngine` provider-API, оставить старый local-флаг как backward-compat |
| 2 | Установить provider в pipeline; запустить регрессию unit-тестов |
| 3 | Удалить local-флаг и `activate/deactivate_kill_switch` из interface |
| 4 | Обновить `Supervisor` listener: cancel-all + alert (без state-sync) |
| 5 | Property-based тест: одновременная kill-switch активация + 1000 concurrent `evaluate()` — все возвращают `EmergencyHalt` |

#### Effort

~2-3 PR, 1-2 недели.

---

## D4 — Fail-fast leverage / margin / hold-mode

### Текущий дефект

`TradingPipeline::start()` вызывает `set_leverage`/`set_margin_mode`/`set_hold_mode`, но при отказе биржи pipeline продолжает работу. Первый ордер уйдёт с дефолтным leverage биржи (например, 20x вместо 5x желаемого) или будет отклонён за hold-mode mismatch.

### Дизайн

1. Добавить в `BitgetFuturesOrderSubmitter::set_leverage` строгий контракт: `bool` с явным маппингом ошибок Bitget.

2. В `TradingPipeline::start()`:
   ```cpp
   auto leverage_setup = [&]() -> Result<void> {
       if (!futures_submitter_->set_hold_mode(product_type, "double_hold"))
           return std::unexpected(TbError::ExchangeAuthFailed);
       if (!futures_submitter_->set_margin_mode(symbol_, "isolated"))
           return std::unexpected(TbError::ExchangeAuthFailed);
       if (!futures_submitter_->set_leverage(symbol_, default_lev, "long"))
           return std::unexpected(TbError::ExchangeAuthFailed);
       if (!futures_submitter_->set_leverage(symbol_, default_lev, "short"))
           return std::unexpected(TbError::ExchangeAuthFailed);
       return {};
   };
   if (!leverage_setup()) {
       supervisor_->enter_degraded_mode(
           "leverage_setup_failed for " + symbol_.get());
       return false;  // pipeline.start() возвращает false
   }
   ```

3. `Supervisor::enter_degraded_mode` уже существует. После degraded-mode `Supervisor::can_open_position` для этого символа должен возвращать `false`. Добавить per-symbol degraded set:
   ```cpp
   void Supervisor::mark_symbol_unsafe(const Symbol& symbol, std::string reason);
   bool Supervisor::is_symbol_unsafe(const Symbol& symbol) const;
   ```
   `try_lock_symbol` отказывает для unsafe-символов.

4. Добавить метрику `leverage_setup_failed_total{symbol, reason}` + alert через `OperationalGuard`.

#### Инварианты

* **Inv-11 (никаких ордеров без leverage/margin-mode setup).** Перейдёт из «soft» в «hard»: при отказе pipeline.start() → false → Supervisor не запускает дальнейших pipeline'ов.

#### Effort

~1 PR, 3-5 дней.

---

## D5 — Замена самописного YAML парсера на yaml-cpp

### Текущий дефект

`config/config_loader.cpp::parse_yaml_flat` понимает только `section.key: value`. Списки (`manual_symbols: [a, b]`, `intervals: [1m, 5m, 1h]`) сериализованы хаками; nested mapping глубже 2 уровней не поддерживаются.

### Дизайн

#### Зависимости

* `yaml-cpp` (Ubuntu 24: `apt install libyaml-cpp-dev`).
* В `CMakeLists.txt`:
  ```cmake
  find_package(yaml-cpp 0.7 REQUIRED)
  target_link_libraries(tb_config PUBLIC yaml-cpp::yaml-cpp)
  ```

#### План миграции

| Этап | Действие | Риск |
|------|----------|------|
| 1 | Добавить `YamlCppLoader : IConfigLoader` рядом с существующим. Покрыть unit-тестами полную загрузку существующего `production.yaml` | LOW |
| 2 | Сравнить `AppConfig` после загрузки старым и новым parser'ом — diff field-by-field, fail-test при расхождении | LOW |
| 3 | Переключить factory `create_config_loader()` на новый — заменить strict mode (unknown keys → error) | MEDIUM |
| 4 | Очистить `config_loader.cpp::parse_yaml_flat` — удалить полностью | LOW |
| 5 | Обновить `production.yaml` если есть workaround'ы для старого парсера (списки в одну строку, …) | LOW |

#### Strict-mode

В новом loader'е — `YAML::Node::IsDefined()` на каждом expected ключе. Все `[[unused]]` или `unknown` логируются и вызывают `TbError::ConfigValidationFailed`.

#### Контракт

`IConfigLoader::load(path) → Result<AppConfig>`:

* **Pre.** `path` существует.
* **Post.** Все поля `AppConfig` либо из YAML, либо из default'ов в `config_types.hpp`. Никаких hidden override в loader.cpp (D-cfg-2 fix).

#### Hash compatibility

`config_hash` (SHA-256 от файла) остаётся источником истины для аудита. После миграции hash файла НЕ меняется (если не редактировать YAML), что упрощает миграцию prod-конфигов.

#### Effort

~2-3 PR, 1 неделя.

---

## D6 — Декомпозиция `TradingPipeline`

### Текущий дефект

`trading_pipeline.hpp` ~580 строк state, ~50 полей. `trading_pipeline.cpp` ~3570 строк. God-object: market data, analytics, ML, leverage, risk, execution, hedge, dual-leg, HTF, trailing-stop, periodic tasks, persistence, telemetry — все принадлежат одному классу.

### Дизайн

#### Целевая архитектура

```
TradingPipeline (orchestrator, ~300 lines)
├── PipelineEngines (struct holds all engine shared_ptrs)
├── HtfTrendTracker        — HTF buffers, REST update, trend computation
├── TradingHistoryStats    — rolling window, win_rate, win_loss_ratio
├── FundingRateTracker     — periodic update
├── TrailingStopManager    — chandelier exit, breakeven, partial TP
├── SnapshotPersister      — periodic portfolio snapshot
├── PeriodicTaskScheduler  — see D2
├── DiagCounters           — gate-by-gate diag tracking + log throttling
├── HedgeStateController   — единая истина для hedge/dual-leg
└── PipelineExternalDeps   — REST, private WS, query adapter
```

Pipeline на каждом тике:

```cpp
void TradingPipeline::on_feature_snapshot(FeatureSnapshot snap) {
    std::lock_guard lock(market_state_mutex_);  // см. D6 sub-fix: split mutex
    auto ctx = build_tick_context(snap);
    
    auto stage_result = run_pipeline_stages(ctx);  // 28 gates
    
    if (stage_result.action == TickAction::Submit) {
        external_deps_->execute(stage_result.intent, stage_result.risk, ...);
    }
    
    htf_tracker_->on_tick(snap);
    trailing_stop_manager_->on_tick(snap);
    history_stats_->on_tick(snap);
    
    diag_counters_->maybe_log(stage_result.blocked_gate);
}
```

#### Sub-fix: split mutex (часть D6)

Текущий `pipeline_mutex_` единый для public WS + private WS:

```cpp
// Старое:
std::mutex pipeline_mutex_;

// Новое:
std::mutex market_state_mutex_;     // public WS path: features, regime, world, decision
std::mutex execution_state_mutex_;  // private WS, fill events, registry
// Cross-cutting reads (e.g., portfolio snapshot) — std::shared_mutex.
```

Каждый компонент-владелец имеет свой mutex; pipeline координирует только межкомпонентные транзакции.

#### План миграции

| Этап | Что выносится | Сторона |
|------|--------------|---------|
| 1 | `HtfTrendTracker` (поля `htf_*`, `compute_htf_trend`, `maybe_update_htf`) | new file `pipeline/htf_trend_tracker.hpp/cpp` |
| 2 | `TradingHistoryStats` (`trade_history_`, `record_trade_for_stats`, `rolling_win_rate`, `rolling_win_loss_ratio`) | new file |
| 3 | `FundingRateTracker` (`current_funding_rate_`, `last_funding_rate_update_ns_`) | new file |
| 4 | `TrailingStopManager` (`highest_price_since_entry_`, `lowest_price_since_entry_`, `update_trailing_stop`, `reset_trailing_state`) | merge with `ExitOrchestrator`? |
| 5 | `DiagCounters` (`diag_*_block_` фолды + `kDiagLogLimit`) | new file |
| 6 | `SnapshotPersister` (`persist_portfolio_snapshot`, `persist_position_event`, periodic `last_snapshot_ns_`) | new file |
| 7 | Удалить дублирование `hedge_*_` полей с `HedgePairManager` | refactor `HedgePairManager` to be sole owner |
| 8 | Split mutex после всех extracts | ⚠ TSAN тесты обязательны |

#### Инварианты

* Все per-symbol атомарные счётчики (`tick_count_`, `running_`, `last_activity_ns_`) **остаются** в `TradingPipeline` (общие для всех под-компонентов).
* **Inv-9 (single owner ордера в registry)** — не затронут.
* **Inv-10 (mutex hierarchy)** — обновляется: `market_state_mutex_` < `execution_state_mutex_` (выясняется по практике, фиксируется в комментарии).

#### Риски

* Без CI на TSAN риск deadlock'а после split mutex реален. **Запретить split mutex без TSAN-сборки в CI.**
* Hedge state дублирование в pipeline + HedgePairManager — особенно опасно: два owner'а хедж-состояния вне sync.

#### Effort

~10-15 PR, 4-6 недель.

---

## D10 — Clock duality (документация)

### Текущее состояние

`OrderFSM::time_in_current_state_ms` использует `std::chrono::steady_clock`; `wall_clock_now` использует `system_clock`. Контракты двух часов не зафиксированы в `clock/IClock`.

### Действие

Documentation only. Внести в `src/clock/claude.md` правила:

* **Wall clock:** `IClock::now()` — для timestamping (`Timestamp` в Position, OrderRecord, …). Не использовать для интервалов.
* **Monotonic clock:** локально через `std::chrono::steady_clock::now()` — только для измерения интервалов (latency, dwell-time, expiry). Не использовать для labels.

При следующей итерации — расширить `IClock` методом `monotonic_ns()` (см. рекомендации в `src/clock/claude.md`).

---

## Итоговый порядок исполнения (рекомендованный roadmap)

| Sprint | Фокус | Дефекты |
|--------|-------|---------|
| 1 | Базовая стабилизация | D1✅, D8✅, D12✅, D13✅, D14✅ (этот PR) |
| 2 | Risk-критичные | D3, D4 |
| 3 | Конфигурация | D5 |
| 4 | Производительность | D2 (параллельно с TSAN-сборкой в CI) |
| 5 | Структурная гигиена | D6 (только после D2 готов) |
| 6 | Полировка | D10 (документация) |

После каждого sprint — обновить `claude.md` (root + module-level), обновить `MEMORY.md` index если архитектура изменилась.

---

## Принципы безопасного рефакторинга в production trading system

1. **Никогда не выпускать в production без TSAN-сборки.** Split mutex (D6) и async REST (D2) без TSAN — преступление.
2. **Каждый PR — один концерн.** Не миксовать D3 и D6 в одном PR.
3. **Snapshot tests** для config (D5) обязательны: загрузка существующего `production.yaml` должна давать идентичный `AppConfig` до/после миграции.
4. **Canary deploy** на staging минимум 24 часа per-PR прежде чем merge в main.
5. **Reconciliation проверка после каждой структурной правки**: `recover_full_state` против реального Bitget testnet должна сходиться.
