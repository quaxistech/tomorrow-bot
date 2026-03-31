# Архитектурный обзор системы Tomorrow Bot

Tomorrow Bot — производственная адаптивная торговая операционная система,
разработанная для биржи Bitget. Система построена по принципам:
- Микромодульность: каждая подсистема независима и заменяема
- Безопасность: ни один секрет не хранится в коде
- Наблюдаемость: метрики, логи, трассировка для всех решений
- Воспроизводимость: все решения можно реплеить и анализировать

---

## Слои архитектуры

### Слой 1: Инфраструктура ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| common | `tb` | Базовые типы, ошибки, результаты |
| config | `tb::config` | Загрузка и валидация конфигурации |
| security | `tb::security` | Управление секретами, маскирование |
| logging | `tb::logging` | Структурированное логирование |
| metrics | `tb::metrics` | Счётчики, gauges, гистограммы |
| health | `tb::health` | Мониторинг здоровья подсистем |
| clock | `tb::clock` | Абстракция времени |
| supervisor | `tb::supervisor` | Жизненный цикл системы |
| platform | `tb::platform` | Информация о платформе |

### Слой 2: Рыночные данные ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| exchange/bitget | `tb::exchange::bitget` | Клиент API Bitget (REST + WebSocket) |
| market_data | `tb::market_data` | Шлюз рыночных данных |
| normalizer | `tb::normalizer` | Нормализация данных биржи |
| order_book | `tb::order_book` | Поддержание L2-стакана ордеров |
| buffers | `tb::buffers` | Lock-free кольцевые буферы |
| indicators | `tb::indicators` | Технические индикаторы (встроенные) |
| features | `tb::features` | Feature Engineering |
| features/advanced_features | `tb::features` | CUSUM, VPIN, Volume Profile, Time-of-Day Alpha |
| pair_scanner | `tb::pair_scanner` | Профессиональный выбор торговых пар (v5: parallel, retry, diversification) |

### Слой 3: Интеллект и стратегии ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| world_model | `tb::world_model` | Мировая модель рынка (9 состояний) |
| regime | `tb::regime` | Классификация рыночных режимов (13 режимов) |
| uncertainty | `tb::uncertainty` | 9-мерная оценка неопределённости (v2) |
| strategy | `tb::strategy` | 9 торговых стратегий (futures-aware) |
| leverage | `tb::leverage` | Адаптивное кредитное плечо 5×–20× |
| strategy_allocator | `tb::strategy_allocator` | Аллокация по режимам и world suitability |
| decision | `tb::decision` | Комитетная агрегация решений с вето |
| ai | `tb::ai` | AI Advisory Engine (интерфейс) |

### Слой 3.5: ML/AI ✅ (новый)

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| ml/bayesian_adapter | `tb::ml` | Bayesian Online Adaptation (Normal-Normal conjugate prior, regime-conditioned) |
| ml/entropy_filter | `tb::ml` | Shannon entropy на 4 каналах, quality gate (порог 0.85) |
| ml/microstructure_fingerprint | `tb::ml` | 5D fingerprint × 5 buckets = 3125 паттернов, knowledge base |
| ml/liquidation_cascade | `tb::ml` | Детектор каскадных ликвидаций (velocity + volume + depth) |
| ml/correlation_monitor | `tb::ml` | Pearson correlation с BTC/ETH (short 20 / long 100 window) |
| ml/thompson_sampler | `tb::ml` | 5-arm Beta bandit для RL entry timing, exponential decay 0.995 |

### Слой 4: Исполнение ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| execution_alpha | `tb::execution_alpha` | Оптимизация исполнения: VPIN + стакан + CUSUM; PostOnly/Passive/Hybrid/Aggressive/NoExecution |
| opportunity_cost | `tb::opportunity_cost` | Opportunity cost scoring: 10 каскадных правил, портфельный контекст, адаптивные пороги, аудит-трасса |
| portfolio | `tb::portfolio` | Управление портфелем, PnL, exposure |
| portfolio_allocator | `tb::portfolio_allocator` | Иерархическая аллокация капитала |
| risk | `tb::risk` | 14 жёстких проверок, kill switch |
| execution | `tb::execution` | FSM ордеров (10 состояний), Paper/Bitget submitter |
| execution/twap_executor | `tb::execution` | Smart TWAP — адаптивное разбиение крупных ордеров (3-10 слайсов) |
| pipeline | `tb::pipeline` | Оркестрация торгового pipeline (staged v2.0: freshness gate, latency SLA, order watchdog, continuous reconciliation) |

### Слой 5: Аналитика и исследования ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| persistence | `tb::persistence` | EventJournal + SnapshotStore |
| replay | `tb::replay` | Детерминированное воспроизведение решений |
| telemetry | `tb::telemetry` | Исследовательская телеметрия (envelope) |
| alpha_decay | `tb::alpha_decay` | Мониторинг деградации альфы |
| shadow | `tb::shadow` | Подсистема виртуального исполнения v2.0; записывает гипотетические решения, симулирует fill quality (slippage + maker/taker fees), отслеживает P&L в конфигурируемых окнах (1с/5с/30с), ведёт multi-leg shadow-позиции, формирует ShadowComparison с divergence_reasons, генерирует алерты. Режимы: Observation, Validation, Discovery. PostgreSQL persistence опционально |
| champion_challenger | `tb::champion_challenger` | A/B тестирование: net P&L, hit rate, drawdown, pre-promotion audit |

### Слой 6: Защита и управление ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| adversarial_defense | `tb::adversarial` | 6 видов угроз, cooldown, severity |
| synthetic_scenarios | `tb::scenarios` | 9 стресс-сценариев с валидацией |
| self_diagnosis | `tb::self_diagnosis` | Объяснение решений (human + machine) |
| governance | `tb::governance` | Audit log, strategy registry, mode tracking |
| operator_control | `tb::operator_control` | 11 команд оператора (kill-switch, shadow, inspect) |

---

## Потоки данных

```
                           PairScanner v5 (REST API)
                                │
                    выбор лучших торговых пар (24ч ротация)
                    ├── parallel candle fetch + retry/circuit breaker
                    ├── scoring v4: Momentum+Trend+Tradability+Quality
                    └── diversification filter (correlation, sectors)
                                │
                                ▼
                    Bootstrap: загрузка истории
                    ├── 200 × 1m свечей → FeatureEngine
                    └── 200 × 1h свечей → HTF Trend Analysis
                                │
Bitget WebSocket                │           Bitget REST API
       │                        ▼                    ▲
       ▼                 TradingPipeline              │
MarketDataGateway ────────────► ExecutionEngine        │
       │                    ┌───► TWAP Executor ──────┘
       ▼                    │         ▲
  Normalizer           RiskEngine (14 checks)
       │                    ▲         │
       ▼                    │    Volatility Targeting
 FeatureEngine        Chandelier Exit / Trailing Stop
       │                    ▲
       ├──► WorldModel      │
       ├──► RegimeEngine   PortfolioAllocator
       ├──► UncertaintyEngine     ▲
       │                          │
       ▼                    OpportunityCostEngine
 AdvancedFeatureEngine            ▲
 (CUSUM, VPIN, VolumeProfile,    │
  Time-of-Day Alpha)       ExecutionAlphaEngine
       │                          ▲
       ▼                          │
 ML Layer                   HTF Trend Filter
 ├── EntropyFilter          Market Readiness Gate
 ├── Fingerprint                  ▲
 ├── CascadeDetector              │
 ├── CorrelationMonitor    DecisionAggregationEngine
 ├── BayesianAdapter              │
 └── ThompsonSampler       FeatureEngine
       │                          │
       └──────────────────────────┘
            Alpha Decay Feedback
            HTF Real-Time Update (каждый час)
```

---

## Принципы проектирования

1. **Без исключений в бизнес-логике**: все ошибки через `Result<T>` (`std::expected`)
2. **RAII повсюду**: никаких голых `new`/`delete`
3. **Strong Types**: `Symbol`, `Price`, `Quantity`, `OrderId` — не голые строки/числа
4. **Секреты через ссылки**: `SecretRef` в конфигурации, не сами секреты
5. **Наблюдаемость**: каждое решение имеет `CorrelationId` для трассировки
6. **Режимы работы**: paper → shadow → testnet → production
7. **Dependency Injection**: все компоненты получают зависимости через конструктор
8. **Config-driven**: переключение Paper/Production через YAML, не пересборку

---

## Улучшения pipeline v2.0

Версия 2.0 вводит четыре фазы профессионального укрепления:

### Phase 0 — Unified Context Architecture
- **`PipelineTickContext`** — единая структура-носитель для всех данных тика
- **`PipelineStageResult`** — типизированные исходы стадий (Pass / Veto / Degrade / Escalate)
- Чёткие границы стадий: Ingress → ML → Market Context → Signals → Decision → Execution

### Phase 1 — Hot-path Discipline
- **`PipelineLatencyTracker`** — трекер P50/P95/P99 по каждой стадии, кольцевой буфер 512 сэмплов
- **Freshness Gate** — отклонение устаревших котировок (порог 5 сек) до любой обработки
- **Backlog Detection** — мониторинг gap между тиками (>2 сек = предупреждение)
- SLA-бюджеты: Ingress 100 мкс, ML 1000 мкс, Context 500 мкс, Signals 2000 мкс, Decision 500 мкс, Risk 300 мкс, Exec 1000 мкс, Total 5000 мкс

### Phase 2 — Execution Hardening
- **`OrderWatchdog`** — периодический мониторинг (каждые 10 сек) всех активных ордеров
- Обрабатывает: PendingAck timeout (5 сек), Open timeout (30 сек), PartiallyFilled timeout (60 сек), UnknownRecovery timeout (10 сек)
- Автоматическая отмена с cancel_callback; эскалация через alert_callback

### Phase 4 — State Durability
- **`BitgetExchangeQueryAdapter`** — мост REST API → `IExchangeQueryService` для reconciliation
- **Continuous Reconciliation** — сравнение внутреннего состояния с биржей каждые 60 секунд
- Детектирует: OrderExistsOnlyLocally, StateMismatch, QuantityMismatch
- Авторазрешение конфликтов через `ReconciliationEngine`

### Обновлённый поток данных

```
Bitget WebSocket → MarketDataGateway → on_feature_snapshot()
                                              │
                                    ┌─────────▼─────────────┐
                                    │   Phase 1: Freshness   │
                                    │   Gate (reject >5s)    │
                                    └─────────┬─────────────┘
                                              │ (fresh)
                                    ┌─────────▼─────────────┐
                                    │   Phase 1: Backlog     │
                                    │   Detection (gap >2s)  │
                                    └─────────┬─────────────┘
                                              │
                                    ┌─────────▼─────────────┐
                                    │   Phase 2/4: Periodic  │
                                    │   Tasks (watchdog,     │
                                    │   reconciliation)      │
                                    └─────────┬─────────────┘
                                              │
                                    [existing pipeline stages]
                                    WorldModel → Regime → Uncertainty
                                    → Strategies → Decision → Risk → Execution
```
