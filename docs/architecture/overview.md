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
| indicators | `tb::indicators` | Технические индикаторы (TA-Lib) |
| features | `tb::features` | Feature Engineering |
| pair_scanner | `tb::pair_scanner` | Автоматический выбор лучших торговых пар |

### Слой 3: Интеллект и стратегии ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| world_model | `tb::world_model` | Мировая модель рынка (9 состояний) |
| regime | `tb::regime` | Классификация рыночных режимов (13 режимов) |
| uncertainty | `tb::uncertainty` | 4-мерная оценка неопределённости |
| strategy | `tb::strategy` | 5 детерминированных торговых стратегий |
| strategy_allocator | `tb::strategy_allocator` | Аллокация по режимам и world suitability |
| decision | `tb::decision` | Комитетная агрегация решений с вето |
| ai | `tb::ai` | AI Advisory Engine (интерфейс) |

### Слой 4: Исполнение ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| execution_alpha | `tb::execution_alpha` | Оптимизация исполнения (passive/aggressive/hybrid) |
| opportunity_cost | `tb::opportunity_cost` | Ранжирование кандидатов, suppress/defer |
| portfolio | `tb::portfolio` | Управление портфелем, PnL, exposure |
| portfolio_allocator | `tb::portfolio_allocator` | Иерархическая аллокация капитала |
| risk | `tb::risk` | 14 жёстких проверок, kill switch |
| execution | `tb::execution` | FSM ордеров (10 состояний), Paper/Bitget submitter |
| pipeline | `tb::pipeline` | Оркестрация торгового pipeline |

### Слой 5: Аналитика и исследования ✅

| Модуль | Пространство имён | Назначение |
|--------|------------------|-----------:|
| persistence | `tb::persistence` | EventJournal + SnapshotStore |
| replay | `tb::replay` | Детерминированное воспроизведение решений |
| telemetry | `tb::telemetry` | Исследовательская телеметрия (envelope) |
| alpha_decay | `tb::alpha_decay` | Мониторинг деградации альфы |
| shadow | `tb::shadow` | Теневой режим с гипотетическим PnL |
| champion_challenger | `tb::champion_challenger` | A/B тестирование стратегий |

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
                          PairScanner (REST API)
                                │
                    выбор лучших торговых пар (24ч ротация)
                                │
                                ▼
                    Bootstrap: загрузка истории
                    ├── 200 × 1m свечей → FeatureEngine
                    └── 200 × 1h свечей → HTF Trend Analysis
                                │
Bitget WebSocket                │           Bitget REST API
       │                        ▼                    ▲
       ▼                 TradingPipeline              │
MarketDataGateway ────────────► ExecutionEngine
       │                              ▲
       ▼                              │
  Normalizer                    RiskEngine (14 checks)
       │                              ▲
       ▼                              │
 FeatureEngine              PortfolioAllocator
       │                              ▲
       ├──► WorldModel                │
       ├──► RegimeEngine        OpportunityCostEngine
       ├──► UncertaintyEngine         ▲
       │                              │
       ▼                        ExecutionAlphaEngine
StrategyAllocator                     ▲
       │                              │
       ├──► Momentum                  │
       ├──► MeanReversion       HTF Trend Filter
       ├──► Breakout            Market Readiness Gate
       ├──► Scalp                     ▲
       └──► VolExpansion              │
                                DecisionAggregationEngine
                                      │
                                FeatureEngine
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
