# Модуль pipeline — подробный разбор текущей реализации

Временный рабочий документ.

Дата: 2026-04-09

## 1. Назначение модуля

`src/pipeline` — это orchestration-слой бота. Именно здесь все остальные подсистемы соединяются в один рабочий торговый цикл:

1. получение рыночных данных;
2. построение feature snapshot;
3. аналитика рынка;
4. генерация торгового сигнала;
5. ML-фильтрация и адаптация;
6. sizing, risk и leverage;
7. исполнение ордера;
8. пост-обработка, watchdog и reconciliation.

Важный момент: текущий pipeline не является набором независимых стадий с явным state machine. В коде это в основном один большой монолитный callback `TradingPipeline::on_feature_snapshot()`, внутри которого последовательно выполняются десятки gate-ов и ветвей.

То есть архитектурно это не "message bus" и не DAG, а **однопоточный orchestration loop на каждый символ**.

## 2. Состав модуля

В каталоге `src/pipeline` находятся:

| Файл | Назначение |
|---|---|
| `trading_pipeline.hpp/.cpp` | основной orchestration-класс `TradingPipeline` |
| `order_watchdog.hpp/.cpp` | watchdog жизненного цикла активных ордеров |
| `pipeline_tick_context.hpp` | заготовка единого контекста тика для staged-pipeline |
| `pipeline_stage_result.hpp` | тип результата одной стадии (`Pass`, `Veto`, `Degrade`, `Escalate`) |
| `pipeline_latency_tracker.hpp` | инфраструктура SLA/P50/P95/P99 по стадиям |
| `pipeline.md` | этот временный технический документ |
| `CMakeLists.txt` | сборка библиотеки `tb_pipeline` |

Собирается статическая библиотека `tb_pipeline`, которая линкуется почти со всеми ключевыми подсистемами бота: market data, features, indicators, world model, regime, uncertainty, strategy, decision, execution, risk, leverage, reconciliation, ML.

## 3. Главный объект: `TradingPipeline`

Один экземпляр `TradingPipeline` обслуживает **один символ**.

Ключевые свойства текущего дизайна:

- pipeline создаётся с конкретным `symbol`;
- внутри конструктора сразу поднимается почти весь dependency graph;
- pipeline жёстко ориентирован на **USDT-M futures only**;
- весь runtime-цикл привязан к входящим `FeatureSnapshot` от `MarketDataGateway`.

### 3.1. Public API

У `TradingPipeline` немного внешнего API:

- `start()` — запуск pipeline;
- `stop()` — остановка pipeline;
- `symbol()` — получить символ;
- `set_symbol_precision()` — установить precision инструмента;
- `set_exchange_rules()` — установить биржевые правила инструмента;
- `set_num_pipelines()` — сообщить, сколько pipeline работают параллельно;
- `is_connected()` — жив ли gateway/WebSocket;
- `has_open_position()` — есть ли открытая позиция;
- `is_idle()` — считается ли pipeline idle для ротации символов;
- `last_activity_time_ns()` — timestamp последней активности.

Внешний интерфейс намеренно узкий. Вся сложность находится внутри callback-а `on_feature_snapshot()`.

### 3.2. Владение зависимостями

`TradingPipeline` сам создаёт и хранит почти все свои зависимости. По сути это composition root для symbol-level runtime.

Основные группы зависимостей:

**Инфраструктура**

- `ISecretProvider`
- `ILogger`
- `IClock`
- `IMetricsRegistry`

**Рыночные данные и признаки**

- `IndicatorEngine`
- `FeatureEngine`
- `LocalOrderBook`
- `MarketDataGateway`
- `AdvancedFeatureEngine`

**Аналитика**

- `RuleBasedWorldModelEngine`
- `RuleBasedRegimeEngine`
- `RuleBasedUncertaintyEngine`

**Стратегии и решения**

- `StrategyRegistry`
- `StrategyEngine`
- `RegimeAwareAllocator`
- `CommitteeDecisionEngine`

**Портфель, sizing, риск, исполнение**

- `InMemoryPortfolioEngine`
- `HierarchicalAllocator`
- `RuleBasedOpportunityCost`
- `ProductionRiskEngine`
- `LeverageEngine`
- `ExecutionEngine`
- `SmartTwapExecutor`

**ML-слой**

- `BayesianAdapter`
- `EntropyFilter`
- `MicrostructureFingerprinter`
- `LiquidationCascadeDetector`
- `CorrelationMonitor`
- `ThompsonSampler`
- `MlSignalSnapshot`

**Биржевая интеграция и восстановление**

- `BitgetRestClient`
- `BitgetFuturesOrderSubmitter`
- `BitgetFuturesQueryAdapter`
- `ReconciliationEngine`
- `OrderWatchdog`

## 4. Что происходит в конструкторе

Конструктор `TradingPipeline` — это не лёгкая инициализация полей. Он фактически поднимает большую часть symbol-runtime.

Порядок инициализации в коде:

1. Проверка futures-only режима.
2. Создание индикаторов и `FeatureEngine`.
3. Создание локального стакана `LocalOrderBook`.
4. Создание аналитического слоя: world model, regime, uncertainty.
5. Создание registry стратегий и регистрация `StrategyEngine`.
6. Создание `RegimeAwareAllocator` и `CommitteeDecisionEngine` с маппингом расширенного decision config из `AppConfig`.
7. Попытка поднять PostgreSQL storage через `POSTGRES_URL`; при неуспехе используется in-memory fallback.
8. Создание `AdvancedFeatureEngine`.
9. Создание всех ML-компонентов.
10. Регистрация двух bayesian-параметров:
	 - `conviction_threshold`;
	 - `atr_stop_multiplier`.
11. Создание `PortfolioEngine` и установка дефолтного плеча.
12. Создание `PortfolioAllocator` с дополнительной адаптацией для микро-аккаунтов.
13. Создание `ExecutionAlphaEngine` из конфигурации.
14. Создание `OpportunityCostEngine` из конфигурации.
15. Создание `ProductionRiskEngine` и маппинг risk config.
16. Создание `LeverageEngine`.
17. Выбор submitter-а:
	 - `PaperOrderSubmitter` в paper mode;
	 - `BitgetFuturesOrderSubmitter` в production.
18. Создание `ExecutionEngine`.
19. Создание `SmartTwapExecutor`.
20. Создание `MarketDataGateway` с callback-ом на `on_feature_snapshot()`.
21. Создание `PipelineLatencyTracker`.
22. Создание `OrderWatchdog` и установка callback-а на cancel.
23. Создание `ReconciliationEngine` и `BitgetFuturesQueryAdapter`, если есть `rest_client_`.
24. Выполнение startup recovery через `RecoveryService`; при ошибке конструктор бросает исключение.

### 4.1. Futures-only enforcement

Это один из самых жёстких инвариантов модуля. В начале конструктора есть прямой guard:

- если `config_.futures.enabled == false`, конструктор бросает `runtime_error`.

То есть pipeline больше не поддерживает спотовый режим на уровне собственного контракта.

### 4.2. Особенность режима `Paper`

Даже в `Paper` mode pipeline всё равно старается поднять `BitgetRestClient`, чтобы:

- получать exchange rules;
- загружать исторические свечи;
- выполнять bootstrap и HTF-обновления;
- при необходимости использовать публичные REST-эндпоинты.

То есть `Paper` здесь означает "не отправляем реальные ордера", а не "не трогаем биржу вообще".

## 5. `start()` и `stop()`

### 5.1. `start()`

Метод `start()` делает следующее:

1. Логирует режим, символ и число стратегий.
2. Если доступен `rest_client_`:
	- синхронизирует баланс с биржей;
	- запрашивает precision инструмента.
3. Если доступен futures submitter:
	- пытается включить `hedge_mode`;
	- пытается установить `margin_mode`;
	- выставляет дефолтное плечо для `long` и `short`.
4. Загружает исторические свечи через `bootstrap_historical_candles()`.
5. Запускает `gateway_->start()`.
6. Устанавливает `running_ = true`.
7. Инициализирует `last_activity_ns_`.

### 5.2. `stop()`

Остановка минималистична:

- атомарно переводит `running_` в `false` через `compare_exchange_strong`;
- останавливает `gateway_`;
- пишет лог.

Явного shutdown choreography для TWAP, watchdog или reconciliation нет. Предполагается, что при остановке gateway новые тики не приходят и pipeline естественно замирает.

### 5.3. `has_open_position()`

`has_open_position()` делает больше, чем просто смотрит в `PortfolioEngine`:

- сначала проверяет локальную позицию по символу;
- затем, если есть `futures_query_adapter_`, дополнительно спрашивает биржу;
- это сделано потому, что для фьючерсов локальное состояние не всегда достаточно надёжно, особенно для short-позиций и recovery-сценариев.

## 6. Вспомогательные подмодули внутри pipeline

### 6.1. Exchange rules / precision

`fetch_symbol_precision()` получает у биржи:

- `quantityPrecision` / `volumePlace`;
- `pricePrecision` / `pricePlace`;
- `minTradeUSDT`;
- `minTradeNum`.

Затем эти правила прокидываются и в futures submitter, и в paper submitter.

### 6.2. Синхронизация баланса

`sync_balance_from_exchange()`:

1. запрашивает USDT equity на Mix account;
2. делит общий капитал на `num_pipelines_`;
3. обновляет `portfolio_->set_capital()`;
4. синхронизирует открытые позиции между биржей и локальным `PortfolioEngine`.

Отдельно реализован recovery-path:

- если на бирже позиция есть, а локально нет, позиция восстанавливается;
- если локально есть, а на бирже нет, локальная позиция очищается как фантомная.

### 6.3. HTF bootstrap и HTF update

Есть два независимых bootstrap-пути:

- `bootstrap_historical_candles()` для 1m истории и прогрева индикаторов;
- `bootstrap_htf_candles()` для 1H истории и расчёта старшего тренда.

1m bootstrap:

- грузит 200 минутных свечей;
- прокармливает их в `FeatureEngine`;
- устанавливает `indicators_warmed_up_ = true`.

HTF bootstrap:

- грузит до 200 часовых свечей;
- сохраняет close/high/low буферы;
- считает EMA20, EMA50, RSI14, MACD histogram, ADX;
- определяет `htf_trend_direction_` и `htf_trend_strength_`.

В runtime `maybe_update_htf()` может обновлять HTF:

- регулярно раз в час;
- экстренно, если цена ушла более чем на `3 × ATR` от последнего HTF close.

### 6.4. Market readiness gate

До начала торговли pipeline требует не просто приход тиков, а выполнение нескольких условий:

1. HTF должен быть валиден.
2. Основные индикаторы рабочего ТФ должны быть валидны.
3. HTF RSI не должен быть в экстремальной зоне `< 15` или `> 85`.

Если условия ухудшаются, ранее полученный `market_ready_` может быть отозван.

### 6.5. Trailing stop и stop management

У pipeline есть собственный богатый state для сопровождения позиции:

- `highest_price_since_entry_`;
- `lowest_price_since_entry_`;
- `current_stop_level_`;
- `breakeven_activated_`;
- `partial_tp_taken_`;
- `close_order_pending_`;
- `initial_position_size_`;
- `current_trail_mult_`;
- `position_entry_time_ns_`.

`update_trailing_stop()` пересчитывает stop-level каждый тик и использует динамический ATR multiplier, зависящий от:

- ADX;
- глубины книги;
- спреда;
- ширины Bollinger Bands;
- давления buy/sell flow.

Это уже не простой фиксированный ATR-stop, а адаптивный Chandelier Exit.

## 7. Основной цикл `on_feature_snapshot()`

Ниже описан **реальный порядок выполнения** в коде.

### 7.1. Вход и сериализация

На вход приходит готовый `FeatureSnapshot` от `MarketDataGateway`.

В начале метода:

1. проверяется `running_`;
2. берётся `std::lock_guard<std::mutex> pipeline_mutex_`;
3. увеличивается `tick_count_`;
4. фиксируется `now_ns`.

То есть весь tick-path сериализован одним мьютексом.

### 7.2. Freshness gate

Если `snapshot.computed_at` задан, pipeline считает возраст тика.

Порог:

- `5 секунд`.

Если тик старше порога:

- логируется `stale tick`;
- инкрементируется метрика `pipeline_stale_ticks_total`;
- дальнейшая обработка полностью прекращается.

### 7.3. Backlog detection

Pipeline следит за разрывом между последовательными тиком:

- если gap больше `2 секунд` после прогрева, логируется предупреждение о возможном backlog.

Это не блокирующий guard, а диагностика перегрузки.

### 7.4. Периодические фоновые задачи

На каждом тике вызывается `run_periodic_tasks(now_ns)`, который запускает:

- order watchdog;
- continuous reconciliation;
- периодическую синхронизацию баланса;
- периодическое обновление funding rate.

Важная архитектурная особенность: **у pipeline нет отдельного фонового scheduler-а**. Все периодические задачи живут поверх входящего потока тиков.

### 7.5. Обновление портфеля и advanced features

Если `mid_price > 0`:

- портфель обновляет текущую цену;
- `AdvancedFeatureEngine` получает `on_tick(price)`.

Потом advanced features заполняют текущий `snapshot` через `fill_snapshot(snapshot)`.

### 7.6. Раннее обновление ML-компонентов

До любой торговой логики pipeline обновляет три ML-подсистемы и записывает результат в `ml_snapshot_`:

1. `EntropyFilter`
2. `LiquidationCascadeDetector`
3. `CorrelationMonitor`

Что важно:

- entropy получает цену, суммарную глубину, спред и aggressive flow;
- cascade получает цену, суммарную глубину и стороны стакана;
- correlation monitor получает **только primary tick**;
- reference feeds в текущем pipeline не подключены.

### 7.7. Абсолютный приоритет: stop-loss

Сразу после раннего ML вызывается `check_position_stop_loss(snapshot)`.

Если он возвращает `true`, тик полностью прекращает дальнейшую торговую обработку.

Это самый приоритетный guard после freshness.

### 7.8. Warmup

Пока не набрано `200` live-тиков, pipeline не торгует.

Это отдельный runtime warmup поверх REST-bootstrap свечей.

### 7.9. Market readiness

Если рынок ещё не признан готовым, pipeline не идёт дальше.

### 7.10. HTF runtime update

После readiness вызывается `maybe_update_htf(snapshot)`.

### 7.11. Два жёстких ML-veto до любой стратегии

Есть два ранних ML-блокера:

1. если `EntropyFilter` считает рынок шумным — тик блокируется;
2. если `LiquidationCascadeDetector` считает каскад вероятным — тик блокируется.

Это hard veto ещё до world model, regime и стратегий.

### 7.12. Pending-entry логика Thompson

Если ранее Thompson выбрал ждать, а не входить сразу, pipeline держит `pending_entry_`.

Логика такая:

- каждый тик уменьшает `wait_periods_remaining`;
- когда счётчик доходит до нуля, исходный intent активируется заново;
- чтобы не попасть в цикл `Wait -> Activate -> Wait`, используется флаг `thompson_bypass`.

### 7.13. TWAP в приоритете над новыми сигналами

Если у `SmartTwapExecutor` есть активный TWAP-план:

1. pipeline не рассматривает новые сигналы;
2. пытается получить следующий slice;
3. прогоняет slice через `ExecutionAlpha`;
4. прогоняет slice через `RiskEngine`;
5. отправляет slice через `ExecutionEngine`;
6. записывает fill в TWAP-план;
7. возвращается без оценки новых стратегий.

То есть активный TWAP полностью монополизирует tick-path до завершения.

### 7.14. World model -> regime -> uncertainty

Дальше начинается аналитическая часть:

1. `world_model_->update(snapshot)`
2. `regime_engine_->classify(snapshot)`
3. `risk_engine_->set_current_regime(regime.detailed)`
4. `uncertainty_engine_->assess(snapshot, regime, world, portfolio_snapshot, ml_snapshot_)`

Важный нюанс: в `uncertainty` передаётся уже заполненный `ml_snapshot_`, но **до вызова `ml_snapshot_.compute_aggregates()`**. То есть uncertainty видит компонентные ML-поля, но не финальные aggregate-поля того же тика.

### 7.15. Жёсткие блокировки по состоянию рынка

До uncertainty уже после regime/world есть ещё два hard block-а для новых входов:

1. `regime.detailed == Chop` и нет открытой позиции;
2. `world.state == ChopNoise` и нет открытой позиции.

Это прямой запрет на вход в шумовом рынке.

### 7.16. Пауза по extreme uncertainty

Если `UncertaintyEngine` рекомендует `NoTrade`, обработка завершается.

### 7.17. Аллокация стратегий и evaluation

Далее pipeline:

1. получает allocation через `strategy_allocator_->allocate(...)`;
2. собирает контекст для каждой активной стратегии;
3. вызывает `evaluate(ctx)`;
4. собирает `TradeIntent` в вектор `intents`.

В strategy context отдельно прокидываются:

- `features`;
- `regime`;
- `world_state`;
- uncertainty-поля;
- HTF trend direction и strength;
- состояние позиции;
- флаг futures;
- признак свежести фида.

Если intent не поставил `correlation_id`, pipeline генерирует его сам.

### 7.18. Committee decision

Если intents есть, pipeline вызывает:

- `decision_engine_->aggregate(symbol, intents, allocation, regime, world, uncertainty, portfolio_snapshot, snapshot)`.

Если комитет не одобрил сделку или не вернул `final_intent`, pipeline завершается.

### 7.19. ML-фильтры после комитета

После получения финального intent-а pipeline делает вторую волну ML-логики.

#### Microstructure fingerprint

Pipeline:

1. создаёт fingerprint из `snapshot`;
2. считает `predict_edge()`;
3. записывает edge и статус в `ml_snapshot_`;
4. если `fp_edge < -0.1`, отклоняет сигнал.

#### Bayesian adaptation

Если накоплено минимум 20 наблюдений:

- pipeline запрашивает адаптированный `conviction_threshold`;
- также получает адаптированный `atr_stop_multiplier`;
- сохраняет их в `ml_snapshot_`.

По факту BayesianAdapter здесь влияет только на порог conviction и telemetry snapshot.

### 7.20. Порог conviction и экстремальные RSI-гейты

Pipeline считает effective threshold как:

- базовый `config_.decision.min_conviction_threshold`
- плюс bayesian adjustment.

Дополнительно есть два hard block-а:

- BUY запрещён при `RSI > 92`;
- SELL запрещён при `RSI < 8`.

Если `intent.conviction < effective_threshold`, сигнал отклоняется.

### 7.21. Предварительный фильтр Risk:Reward

Для новых входов pipeline сам оценивает минимальный R:R:

- риск = `ATR * atr_stop_multiplier`;
- reward = `ATR * partial_tp_atr_threshold`;
- reward усиливается сильным ADX и совпадением с HTF трендом;
- при слабом ADX слегка штрафуется.

Если вычисленный `rr_ratio` ниже `min_risk_reward_ratio`, сделка пропускается.

### 7.22. Корреляционный risk multiplier

Pipeline повторно вызывает `correlation_monitor_->evaluate()` и вытаскивает `risk_multiplier`, который дальше участвует в sizing через `combined_size_mult`.

В текущем коде это работает не полностью, потому что reference data pipeline не подаёт.

### 7.23. Thompson Sampling

Thompson может выбрать одно из действий:

- `EnterNow`;
- `WaitOnePeriod`;
- `WaitTwoPeriods`;
- `WaitThreePeriods`;
- `Skip`.

Поведение:

- `Skip` сразу отклоняет сделку;
- `Wait*` создаёт `pending_entry_` и завершает тик;
- `EnterNow` пропускает сигнал дальше.

Есть отдельная anti-spam логика:

- если подряд накопилось `50` решений `WaitOnePeriod`, pipeline принудительно помечается idle для последующей ротации пары.

### 7.24. `ml_snapshot_.compute_aggregates()`

Лишь после Thompson pipeline вызывает `ml_snapshot_.compute_aggregates()`.

Это означает:

- aggregate ML snapshot считается поздно;
- uncertainty в этом же тике уже рассчитана раньше.

### 7.25. Финальный HTF barrier

Даже после комитета и ML pipeline накладывает ещё один фильтр:

- BUY блокируется против сильного HTF downtrend;
- SELL блокируется против сильного HTF uptrend;
- close-ордера и reduce-сигналы не должны блокироваться как обычные контртрендовые входы.

Это последний барьер до sizing/execution ветви.

### 7.26. Cooldown-и

До отправки заявки есть два cooldown-а:

1. обычный `order_cooldown_seconds`;
2. отдельный cooldown после стоп-лосса `stop_loss_cooldown_seconds`.

### 7.27. Классификация intent-а: open vs close

Дальше начинается очень важная futures-логика.

Pipeline сам решает, что именно означает финальный intent:

- открытие новой позиции;
- полное закрытие;
- частичное сокращение;
- стратегический exit;
- некорректный exit без позиции.

Для этого используются:

- текущие позиции в портфеле;
- `intent.signal_intent`;
- `current_position_side_`.

Здесь pipeline:

- выставляет `trade_side = Open/Close`;
- выставляет `position_side = Long/Short`;
- при закрытии корректирует `intent.side` под futures semantics:
  - закрытие long = `Sell`;
  - закрытие short = `Buy`.

Дополнительно есть:

- minimum hold time для неэкстренного закрытия;
- minimum conviction для стратегического exit;
- PnL-gate, который не даёт стратегии закрыть небольшую отрицательную позицию, оставляя решение trailing stop-у.

### 7.28. Execution alpha

Дальше pipeline один раз считает `exec_alpha = execution_alpha_->evaluate(intent, snapshot, uncertainty)`.

Эта оценка используется и в ветви закрытия, и в ветви нового входа.

### 7.29. Ветвь закрытия позиции

Если intent классифицирован как закрытие:

1. `ExecutionAlpha` принудительно переводится в `Aggressive`;
2. закрытие всегда считается срочным;
3. размер позиции дополнительно синхронизируется с биржей через `futures_query_adapter_`;
4. если размер слишком мал, закрытие пропускается;
5. `risk_decision` формируется напрямую как approved;
6. `close_order_pending_ = true`.

То есть обычный sizing/risk pipeline для закрытия позиции обходится почти полностью. Закрытие трактуется как аварийно-важный путь.

### 7.30. Ветвь нового входа

Если это новый вход:

1. если `close_order_pending_ == true`, вход запрещается;
2. вызывается `OpportunityCostEngine`;
3. затем `PortfolioAllocator::compute_size()`;
4. затем `RiskEngine::evaluate()`.

Если sizing или risk не одобряют сделку:

- сделка блокируется;
- активные стратегии получают `notify_entry_rejected()`.

### 7.31. Smart TWAP activation

После sizing/risk pipeline может не отправить ордер сразу, а создать TWAP-план, если:

- этого хочет `ExecutionAlpha` через `slice_plan`, или
- этого хочет сам `SmartTwapExecutor`.

В этом случае текущий тик заканчивается, а первый слайс отправится уже на следующем тике.

### 7.32. LeverageEngine

Для открывающего ордера pipeline считает `LeverageContext`, куда входят:

- regime;
- uncertainty;
- ATR-normalized volatility;
- drawdown;
- adversarial severity;
- conviction;
- funding rate;
- side и entry price.

Отдельно собирается `adversarial_severity` как composite-сигнал из:

- состояния world model;
- спреда;
- instability;
- VPIN;
- aggressive flow;
- отрицательного fingerprint edge.

Если leverage decision небезопасен, сделка отклоняется.

Если это production и плечо существенно изменилось:

- pipeline вызывает `futures_submitter_->set_leverage()` на бирже;
- только потом продолжает путь к отправке ордера.

### 7.33. Anti-fingerprinting

Перед отправкой ордера для **новых входов** pipeline вносит два типа маскировки:

1. шум по размеру ордера `±2%`;
2. случайный time jitter `50-300 ms`.

Для stop-loss и закрытий это не используется, потому что там важнее точность и срочность.

### 7.34. Отправка ордера и post-order state update

Перед отправкой pipeline:

- обновляет `last_order_time_ns_`;
- обновляет `last_activity_ns_`.

Дальше вызывается:

- `execution_engine_->execute(intent, risk_decision, exec_alpha, uncertainty)`.

Если ордер успешно отправлен:

- сбрасывается `consecutive_rejections_`;
- `risk_engine_->record_order_sent()`;
- логируется `ОРДЕР ОТПРАВЛЕН`.

Если это **новая позиция**, pipeline дополнительно:

- сохраняет `current_position_strategy_`;
- сохраняет `current_position_side_`;
- сохраняет `current_position_conviction_`;
- сохраняет `current_entry_thompson_action_`;
- сохраняет estimated slippage;
- сбрасывает MAE;
- сохраняет `last_exec_alpha_`;
- сбрасывает trailing state и инициализирует entry anchors;
- сохраняет `last_entry_fingerprint_`.

Если это **закрытие позиции**, pipeline:

- записывает fingerprint outcome;
- передаёт наблюдение в `BayesianAdapter`;
- записывает reward в `ThompsonSampler`;
- сообщает результат в `RiskEngine`;
- пишет rolling trade stats;
- обновляет edge stats в `LeverageEngine`;
- сбрасывает trailing state.

### 7.35. Ошибка исполнения и backoff

Если `execute()` вернул ошибку:

- увеличивается `consecutive_rejections_`;
- для close-order снимается `close_order_pending_`;
- применяется экспоненциальный backoff на основе количества подряд неудач;
- стратегии получают `notify_entry_rejected()`.

## 8. Как работает stop-loss subsystem

Stop subsystem живёт внутри pipeline и не делегирован отдельному модулю.

### 8.1. Сигналы на закрытие внутри `check_position_stop_loss()`

Pipeline может закрыть позицию без участия стратегий по одному из шести триггеров:

1. `quick_profit_triggered`
2. `trailing_stop_triggered`
3. `partial_tp_triggered`
4. `fixed_stop_triggered`
5. `price_stop_triggered`
6. `time_exit_triggered`

### 8.2. Quick profit

Если позиция уже в хорошем плюсе, прошло минимум заданное время и профит заметно перекрывает round-trip fee, pipeline может закрыть позицию целиком как `QUICK_PROFIT`.

### 8.3. Partial TP

Если прибыль достигла `N × ATR`, pipeline пытается закрыть долю позиции.

При этом есть anti-dust логика:

- если остаток после partial close слишком мал, pipeline закрывает всю позицию целиком.

### 8.4. Fixed stop и price stop

Есть два независимых аварийных предохранителя:

- стоп по доле потери от капитала;
- стоп по проценту движения цены против входа.

### 8.5. Time exit

Есть два time-based выхода:

- max hold для убыточной позиции;
- абсолютный max hold.

Оба могут быть немного продлены, если momentum остаётся в пользу позиции.

### 8.6. Как исполняется stop order

Когда stop/TP сработал:

1. pipeline проверяет отдельный `kStopLossCooldownNs = 5s`;
2. синхронизирует фактический размер позиции с биржей;
3. при фантомной позиции очищает портфель и state;
4. формирует `close_intent` вручную;
5. жёстко ставит:
	- `trade_side = Close`;
	- корректный `position_side`;
	- `conviction = 1.0`;
	- `urgency = 1.0`.
6. полностью bypass-ит `ExecutionAlpha` через ручной aggressive result;
7. вызывает `execution_engine_->execute()`.

Ключевой смысл: stop-loss не должен зависеть от обычных условий входа и не должен быть заблокирован execution heuristics.

### 8.7. Важная асимметрия с обычным close path

При успешном полном стратегическом закрытии pipeline пишет наблюдение в:

- fingerprinter;
- bayesian adapter;
- thompson sampler.

При успешном stop-loss path pipeline пишет только в:

- fingerprinter;
- thompson sampler;
- risk engine.

`BayesianAdapter` в stop-loss ветке **не обновляется**.

Это важная особенность текущей реализации.

## 9. Периодические фоновые механизмы

### 9.1. OrderWatchdog

`OrderWatchdog` запускается через `run_order_watchdog()` не чаще, чем раз в `10 секунд`.

Он:

1. получает список активных ордеров из `ExecutionEngine`;
2. дополнительно вызывает `cancel_timed_out_orders()`;
3. классифицирует каждый ордер;
4. при необходимости отменяет ордер или эскалирует проблему;
5. возвращает список отчётов для логирования.

Классификация ордеров:

- `PendingAck` старше `5s` -> `Cancel`
- `UnknownRecovery` старше `10s` -> `RecoverState`
- `PartiallyFilled` старше `60s` -> `ForceClose`
- `Open` / `CancelPending` старше `30s` -> `Cancel`

После проверки pipeline также чистит terminal orders старше `1 часа`.

### 9.2. Continuous reconciliation

`run_continuous_reconciliation()` запускается не чаще, чем раз в `60 секунд`.

Если есть активные ордера:

- вызывается `reconcile_active_orders(active_orders)`;
- расхождения логируются.

### 9.3. Balance sync

Раз в `5 минут` pipeline синхронизирует USDT equity и перераспределяет капитал между параллельными pipeline.

### 9.4. Funding rate update

Раз в `5 минут` pipeline запрашивает funding rate и передаёт его в `RiskEngine`.

## 10. Состояние и модель конкурентности

### 10.1. Модель конкурентности

Текущий pipeline почти полностью основан на модели:

- один symbol;
- один callback;
- один мьютекс `pipeline_mutex_`;
- входящие тики сериализуются.

Это упрощает reasoning о состоянии, но делает `on_feature_snapshot()` очень длинным и чувствительным к latency spikes.

### 10.2. Atomic state

Atomic используются только для простых жизненных флагов:

- `running_`;
- `last_activity_ns_`.

Остальное состояние защищено только тем, что tick-path последовательно проходит под `pipeline_mutex_`.

### 10.3. Ключевое mutable state

Самые важные runtime-поля:

- счётчики тиков и отказов;
- pending-entry Thompson state;
- trailing stop state;
- HTF state и буферы;
- текущая позиционная мета-информация;
- cooldown timestamps;
- last applied leverage;
- rolling trade history;
- `ml_snapshot_`.

## 11. Что из pipeline-модуля реально используется, а что пока заготовка

### 11.1. Реально используется сейчас

- `TradingPipeline`
- `OrderWatchdog`

### 11.2. Подготовленная, но не интегрированная staged-инфраструктура

В модуле уже лежат полезные абстракции под более формальный staged-pipeline:

- `PipelineTickContext`
- `PipelineStageResult`
- `stage_pass()/stage_veto()/stage_degrade()/stage_escalate()`
- `StageTimer`
- `PipelineLatencyTracker`

Но в текущем коде:

- `PipelineTickContext` не протаскивается через `on_feature_snapshot()`;
- `PipelineStageResult` не используется для явного описания исходов стадий;
- `StageTimer` нигде не применяется;
- `PipelineLatencyTracker` создаётся, но не видно реальной записи стадий и `emit_metrics()` в runtime;
- helper `check_quote_freshness()` существует, но фактическая freshness-проверка написана inline внутри `on_feature_snapshot()`.

Иными словами, staged-архитектура уже намечена, но текущая реализация остаётся монолитной.

## 12. Конфигурационно-зависимое поведение

Pipeline очень сильно зависит от конфигурации. Особенно важны:

### 12.1. `config_.futures`

- futures-only enforcement;
- `product_type` для REST-запросов;
- `default_leverage` и `max_leverage`;
- `margin_mode`;
- настройки `LeverageEngine`.

### 12.2. `config_.trading`

- `mode` (`Paper` vs production);
- `initial_capital`;
- при маленьком капитале pipeline ослабляет часть ограничений sizing.

### 12.3. `config_.trading_params`

Именно здесь находятся ключевые runtime-гейты по торговле:

- cooldown;
- stop-loss cooldown;
- ATR stop multiplier;
- thresholds для breakeven и partial TP;
- min/max hold;
- dust threshold;
- risk:reward minimum;
- quick profit settings;
- price stop loss threshold.

### 12.4. `config_.decision`

Определяет поведение committee aggregation и conviction threshold.

### 12.5. `config_.opportunity_cost`

Определяет suppress/defer/execute-поведение перед sizing.

### 12.6. `config_.risk`

Настраивает `ProductionRiskEngine`, включая:

- daily loss;
- drawdown;
- concentration;
- regime-aware limits;
- trade frequency limits.

## 13. Наиболее важные инженерные наблюдения

### 13.1. Pipeline очень мощный, но сильно монолитный

`on_feature_snapshot()` совмещает:

- ingestion guards;
- ML-обновление;
- сопровождение позиции;
- signal generation;
- risk/sizing;
- leverage;
- execution;
- post-trade learning.

Это удобно для локального reasoning, но затрудняет модульное тестирование и точный SLA-профилинг.

### 13.2. Stop subsystem — это отдельный mini-engine внутри pipeline

Практически вся логика управления живой позицией находится прямо в pipeline, а не в отдельном position manager.

### 13.3. Вход и выход идут по асимметричным путям

Открытие новой позиции проходит через:

- opportunity cost;
- sizing;
- risk;
- leverage;
- anti-fingerprinting.

Закрытие позиции проходит по существенно более короткому и жёсткому пути.

Это сделано осознанно: для close path приоритетнее гарантированное исполнение, чем аккуратный selection.

### 13.4. Correlation monitor интегрирован лишь частично

Pipeline обновляет только primary side корреляционного монитора. Reference asset feeds в текущем коде не подаются.

Следствие:

- корреляционный компонент существует в архитектуре;
- его статус и поля snapshot заполняются;
- но полноценный cross-asset контроль без reference data не достигается.

### 13.5. Bayesian feedback-контракт пока нестрогий

При обычном закрытии pipeline передаёт в `BayesianAdapter` raw `closing_pnl`, а не нормализованный reward.

### 13.6. Aggregate ML snapshot вычисляется поздно

`ml_snapshot_.compute_aggregates()` вызывается после большинства upstream-решений, поэтому aggregate-поля не участвуют в uncertainty-модели того же тика.

### 13.7. SLA-инфраструктура пока не доведена до полного runtime-использования

Техническая база для staged latency tracking уже есть, но реальный код пока ею почти не пользуется.

## 14. Краткий итог

Текущий `pipeline` — это центральный symbol-runtime бота, который:

- жёстко работает в futures-only модели;
- стартует почти все подмодули сам;
- исполняет торговую логику в одном большом tick-driven callback;
- сочетает market analytics, ML, sizing, risk, leverage, execution и recovery;
- отдельно и очень глубоко управляет уже открытой позицией через встроенный stop subsystem.

Если кратко, это **не просто конвейер маршрутизации**, а полноценный orchestration engine, где на одном уровне сосуществуют:

- signal generation;
- veto/gating;
- stateful trade management;
- exchange synchronization;
- online learning;
- protection/recovery.

Для текущего проекта это фактически главный runtime-центр логики на уровне одного инструмента.
