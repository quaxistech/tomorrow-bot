# Архитектура

## Обзор

Tomorrow Bot — модульная система алгоритмической торговли для Bitget USDT-M Futures.
35 модулей, C++23, tick-driven архитектура.

Система состоит из трёх уровней:

1. **ScannerEngine** — выбор торговых пар
2. **TradingPipeline** — конвейер обработки тика (один на символ)
3. **Supervisor** — управление жизненным циклом

---

## Потоки данных

```
                    Bitget REST API
                         │
                   ScannerEngine
                    (top-N пар)
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
    Pipeline #1    Pipeline #2    Pipeline #N
    (BTCUSDT)      (ETHUSDT)     (...)
          │
          ▼
    Bitget WebSocket
    (ticker, trade, book, candle)
          │
          ▼
    Normalizer → OrderBook → FeatureEngine
                                   │
                 ┌─────────────────┼──────────────┐
                 ▼                 ▼               ▼
           WorldModel        RegimeEngine    UncertaintyEngine
           (9 состояний)     (13 режимов)   (9 измерений)
                 │                 │               │
                 └────────┬────────┘               │
                          ▼                        │
                 StrategyAllocator ◀───────────────┘
                          │
                 StrategyEngine
                 (4 скальп-сценария)
                          │
                 DecisionEngine
                 (conviction, вето)
                          │
              ┌───────────┼───────────┐
              ▼           ▼           ▼
        BayesianAdapter  Entropy   Thompson
        Fingerprint      Cascade   Correlation
              │
              ▼
    ExecutionAlpha → OpportunityCost → PortfolioAllocator
              │
              ▼
       LeverageEngine (1×–20×)
              │
              ▼
       RiskEngine (33 проверки)
              │
              ▼
       ExecutionEngine → TWAP → Bitget REST API
              │
              ▼
       Reconciliation ◀──▶ Portfolio
```

---

## Модули

### Инфраструктура

| Модуль | Описание |
|--------|----------|
| `common` | Базовые типы (`StrongType`, `Result<T>`, `Error`), перечисления, exchange rules |
| `config` | Загрузка YAML, валидация полей, хеш конфигурации |
| `security` | `EnvSecretProvider` — загрузка ключей из `.env`, маскирование секретов |
| `logging` | `ILogger` — структурированный JSON-логгер, уровни DEBUG–FATAL |
| `metrics` | `IMetricsRegistry` — Prometheus-совместимые counter/gauge/histogram |
| `clock` | `IClock` — абстракция времени (`SystemClock` / `TestClock` для тестов) |
| `supervisor` | Жизненный цикл подсистем, SIGTERM/SIGINT, symbol locks, kill switch broadcast |

### Рыночные данные

| Модуль | Описание |
|--------|----------|
| `exchange/bitget` | REST + WebSocket клиенты, HMAC signing, Futures submitter/query |
| `market_data` | `MarketDataGateway` — WS-подписки, callback на снимки |
| `normalizer` | Нормализация сырых данных Bitget → внутренние структуры |
| `order_book` | Локальный L2-ордербук, bid/ask, спред, глубина |
| `buffers` | Кольцевые буферы для тиков |

### Анализ

| Модуль | Описание |
|--------|----------|
| `indicators` | 12+ индикаторов: SMA, EMA, RSI, MACD, BB, ATR, ADX, OBV, VWAP, ROC, Z-Score |
| `features` | `FeatureEngine` + `AdvancedFeatureEngine` (CUSUM, VPIN, Volume Profile, ToD Alpha) |
| `world_model` | 9 состояний мира, fragility, tendency |
| `regime` | 13 режимов рынка с confidence и stability |
| `uncertainty` | 9-мерная неопределённость: regime, signal, data, execution, portfolio, ML, correlation, transition, operational |

### Торговля

| Модуль | Описание |
|--------|----------|
| `scanner` | `ScannerEngine` — data collection → features → trap detection → filter → rank → bias detection |
| `strategy` | `StrategyEngine` — 4 скальп-сценария, state machine (Idle → Candidate → ... → Cooldown) |
| `strategy_allocator` | Распределение весов стратегий по режиму и world suitability |
| `decision` | `CommitteeDecisionEngine` — conviction threshold, conflict resolution, regime-adaptive |
| `ml` | 6 ML-компонентов (см. ниже) |
| `leverage` | `LeverageEngine` — 7 множителей (base, volatility, drawdown, conviction, funding, adversarial, uncertainty) |

### Исполнение

| Модуль | Описание |
|--------|----------|
| `execution_alpha` | Passive/Aggressive/Hybrid execution стиль по условиям |
| `opportunity_cost` | Ранжирование торговых кандидатов |
| `portfolio` | Позиции, PnL, exposure, cash ledger |
| `portfolio_allocator` | Иерархический сайзинг позиций |
| `risk` | 33 policy-проверки (kill switch, лимиты, cooldown, regime-scaled, uncertainty-aware) |
| `execution` | FSM ордеров (10 состояний), TWAP executor, paper/live submitter |
| `pipeline` | `TradingPipeline` — оркестрация всех компонентов на каждом тике |

### Надёжность

| Модуль | Описание |
|--------|----------|
| `reconciliation` | Сверка ордеров/позиций/баланса с биржей при рестарте |
| `recovery` | Snapshot restore + WAL replay + exchange sync |
| `resilience` | Circuit breaker, retry executor (exponential backoff), idempotency manager |
| `persistence` | PostgreSQL: EventJournal (append-only), SnapshotStore, WAL writer |

---

## ML-компоненты

| Компонент | Назначение |
|-----------|------------|
| `BayesianAdapter` | Онлайн адаптация параметров стратегий (Normal-Normal conjugate prior) |
| `EntropyFilter` | Блокировка при высоком шуме (Shannon entropy > 0.85 на 4 каналах) |
| `MicrostructureFingerprinter` | 5D fingerprint × 5 buckets = 3125 паттернов, блокировка при edge < -0.1 |
| `LiquidationCascadeDetector` | Детекция каскадных ликвидаций (velocity + volume + depth), порог 0.6 |
| `CorrelationMonitor` | Pearson correlation с BTC/ETH, risk_mult 0.5 при decorrelation |
| `ThompsonSampler` | 5-arm Beta bandit для оптимизации момента входа |

Все компоненты агрегируются в `MlSignalSnapshot` — единую структуру для pipeline.

---

## ScannerEngine

Pipeline сканера:

1. **Data Collection** — загрузка тикеров и свечей через Bitget REST
2. **Feature Calculation** — объём, спред, волатильность, momentum
3. **Trap Detection** — обнаружение pump/dump, exhausted moves
4. **Filtering** — отсев по min volume, max spread, blacklist
5. **Ranking** — скоринг и сортировка
6. **Bias Detection** — проверка на смещения в выборке

Результат: top-N пар с полным score breakdown.  
Ротация: фоновый поток, интервал настраивается в конфиге.

---

## Supervisor

Состояния: `Initializing → Starting → Running → Degraded → ShuttingDown → Stopped`

Ключевые функции:

- **Регистрация подсистем** — `register_subsystem(name, start_fn, stop_fn)`
- **Symbol Lock Registry** — эксклюзивная блокировка символа между pipeline
- **Kill Switch Broadcast** — мгновенное уведомление всех pipeline через callback
- **Global Position Limits** — атомарная проверка перед открытием позиции
- **Graceful Shutdown** — таймаут 30с, force-continue при превышении

---

## Strategy Engine

Единая скальпинг-стратегия с 4 сценариями:

| Сценарий | Описание |
|----------|----------|
| Momentum Continuation | Продолжение после микро-консолидации |
| Retest | Ретест пробитого уровня |
| Pullback in Microtrend | Откат внутри микротренда |
| Rejection | Отказ от уровня |

**State Machine**: Idle → Candidate → SetupForming → SetupPendingConfirmation → EntryReady → EntrySent → PositionOpen → PositionManaging → ExitPending → Cooldown → Blocked

Компоненты: `StrategyStateMachine`, `MarketContextEvaluator`, `SetupDetector`, `SetupValidator`, `PositionManager`

---

## Decision Engine

`CommitteeDecisionEngine` — агрегация торговых намерений:

```cpp
DecisionRecord aggregate(
    symbol, intents, allocation, regime, world, uncertainty,
    portfolio /* optional */, features /* optional */
);
```

Функции:
- Regime-adaptive conviction threshold
- Time decay с настраиваемым half-life
- Ensemble conviction bonus
- Drawdown protection
- Execution cost modeling

Причины отклонения: `GlobalUncertaintyVeto`, `SignalConflict`, `LowConviction`, `PortfolioIncompatible`, `ExecutionCostTooHigh`, `DrawdownProtection` и др.

---

## Execution Engine

Модульный оркестратор исполнения:

| Подсистема | Назначение |
|------------|------------|
| `OrderRegistry` | Хранение ордеров, FSM, дедупликация |
| `ExecutionPlanner` | Intent → plan (тип ордера, таймаут, fallback) |
| `FillProcessor` | Обработка исполнений, обновление портфеля |
| `CancelManager` | Отмена по таймауту, контексту, аварии |
| `RecoveryManager` | Сверка, восстановление состояния |
| `ExecutionMetrics` | Телеметрия исполнения |

FSM ордера: 10 состояний (Pending → Submitted → Open → PartiallyFilled → Filled → ...).

`SmartTwapExecutor` — разбиение крупных ордеров на 3–10 слайсов с адаптивным интервалом.

---

## Leverage Engine

Адаптивное кредитное плечо 1×–20× на основе 7 факторов:

| Фактор | Описание |
|--------|----------|
| Base | Базовое плечо по режиму рынка |
| Volatility | ATR/price ratio |
| Drawdown | Текущая просадка |
| Conviction | Уверенность сигнала |
| Funding Rate | Штраф при высоком фандинге |
| Adversarial | Severity рыночных угроз |
| Uncertainty | Уровень неопределённости |

Возвращает `LeverageDecision`: leverage, liquidation price, liquidation buffer %, is_safe, rationale.

---

## Exchange Integration

Единственная биржа — **Bitget USDT-M**:

| Компонент | Описание |
|-----------|----------|
| `BitgetRestClient` | REST API с HMAC-SHA256 signing |
| `BitgetWsClient` | WebSocket для real-time данных |
| `BitgetOrderSubmitter` | Spot-ордера (IOrderSubmitter) |
| `BitgetFuturesOrderSubmitter` | Futures-ордера (Mix API v2) |
| `BitgetExchangeQueryAdapter` | Spot: балансы, позиции |
| `BitgetFuturesQueryAdapter` | Futures: позиции, балансы, ордера |

Hedge mode, isolated margin. Leverage синхронизируется при каждом изменении.

---

## Persistence

PostgreSQL-хранилище:

| Таблица | Назначение |
|---------|------------|
| `tb_journal` | Append-only журнал событий (тип, стратегия, символ, payload) |
| `tb_snapshots` | Снапшоты состояния (ключ → payload) |

**WAL (Write-Ahead Logging)**: критические действия записываются ДО выполнения.  
При crash: незавершённые записи обнаруживаются и обрабатываются при рестарте.

**Recovery**: snapshot restore → WAL replay → exchange sync.

---

## Безопасность

- API-ключи из `.env` через `EnvSecretProvider`, маскирование в логах
- Production Guard: требует env `TOMORROW_BOT_PRODUCTION_CONFIRM`
- Kill switch broadcast — мгновенная остановка всех pipeline
- 33 независимых проверки перед каждым ордером
- Idempotent order submission (стабильный ClientOrderId + дедупликация)
- Write-ahead logging для crash recovery
- Circuit breaker + exponential backoff
- Graceful shutdown по SIGTERM/SIGINT
