# Tomorrow Bot — Adaptive Trading System

Производственная адаптивная торговая операционная система для биржи Bitget.
Написана на C++23, развёртывается на Ubuntu 24.04.

**Версия**: 0.2.0 | **Статус**: Production | **Тесты**: 203/203

---

## Обзор

Tomorrow Bot — модульная система алгоритмической торговли полного цикла:
от автоматического выбора лучшей торговой пары и загрузки исторических данных,
через анализ рынка на нескольких таймфреймах, до исполнения ордеров через REST API.
Акцент на безопасность, наблюдаемость, воспроизводимость и адаптивность.

```
┌─────────────────────────────────────────────────────────────────────┐
│   PairScanner (Bitget REST API) — выбор лучших торговых пар         │
│     анализ объёма, спреда, волатильности → top-N пар                │
│     ротация каждые 24 часа                                         │
└──────────────────────────┬──────────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│   Bootstrap: загрузка истории                                       │
│     200 × 1m свечей (3+ часа) — прогрев индикаторов                │
│     200 × 1h свечей (8+ дней) — HTF тренд-анализ                   │
└──────────────────────────┬──────────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│   Bitget WebSocket (wss://ws.bitget.com/v2/ws/public)               │
│     ticker · trade · books · candle1m · candle5m                    │
└──────────────────────────┬──────────────────────────────────────────┘
                           ▼
    MarketDataGateway → Normalizer → FeatureEngine
                                        │
              ┌─────────────────────────┬┴──────────────────────┐
              ▼                         ▼                       ▼
        WorldModel              RegimeEngine           UncertaintyEngine
              │                         │                       │
              └──────────┬──────────────┘                       │
                         ▼                                      │
              StrategyAllocator ←────────────────────────────────┘
                         │
          ┌──────────────┼──────────────────────────────┐
          ▼              ▼              ▼        ▼       ▼
      Momentum    MeanReversion    Breakout   Scalp  VolExpansion
          │              │              │        │       │
          └──────────────┴──────┬───────┴────────┘       │
                                ▼                        │
              DecisionAggregationEngine ←────────────────┘
                                │
                       ┌────────┤
                       ▼        ▼
              HTF Trend Filter · Market Readiness Gate
                       │
                    ┌──┴────────┬───────────┐
                    ▼           ▼           ▼
            ExecutionAlpha  OpportunityCost  PortfolioAllocator
                    │           │           │
                    └───────────┴─────┬─────┘
                                      ▼
                                RiskEngine (14 проверок)
                                      │
                                      ▼
                            ExecutionEngine → Bitget REST API
                                      │
                                      ▼
                               PortfolioEngine (PnL)
```

**36 модулей** · **5 стратегий** · **14 проверок риска** · **10 состояний FSM ордера** · **HTF тренд-фильтр**

---

## Требования

| Компонент | Версия |
|-----------|--------|
| **Компилятор** | GCC 13+ (C++23) |
| **CMake** | 3.25+ |
| **Boost** | 1.83+ (system, json, thread, beast) |
| **OpenSSL** | 3.x |
| **Catch2** | v3 (подтягивается через FetchContent) |
| **ОС** | Ubuntu 24.04 LTS |

### Установка зависимостей

```bash
sudo apt install -y \
    build-essential cmake \
    libboost-all-dev \
    libssl-dev
```

---

## Быстрый старт

### Сборка

```bash
# Debug-сборка
mkdir build-check && cd build-check
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)

# Release-сборка (с оптимизациями)
mkdir build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Тестирование

```bash
cd build-check
ctest --output-on-failure -j8   # 203 теста
```

### Конфигурация API-ключей

Создайте файл `.env` в корне проекта:

```bash
BITGET_API_KEY=ваш_api_key
BITGET_API_SECRET=ваш_secret_key
BITGET_PASSPHRASE=ваш_passphrase
```

Бот автоматически загружает `.env` при запуске. Файл защищён `.gitignore`.

### Запуск

```bash
# Все команды выполняются из корня проекта

# Бумажная торговля (симуляция, без реальных ордеров)
./build-check/src/app/tomorrow-bot -c configs/paper.yaml

# Реальная торговля (РЕАЛЬНЫЕ ДЕНЬГИ!)
./build-check/src/app/tomorrow-bot -c configs/production.yaml
```

Остановка: `Ctrl+C` (graceful shutdown через SIGINT/SIGTERM).

---

## Режимы торговли

| Режим | Конфиг | Описание |
|-------|--------|----------|
| `paper` | `configs/paper.yaml` | Симуляция — ордера не отправляются на биржу |
| `shadow` | `configs/shadow.yaml` | Теневые расчёты стратегий, сравнение с рынком |
| `testnet` | `configs/testnet.yaml` | Тестовая сеть Bitget (sandbox API) |
| `production` | `configs/production.yaml` | **Реальная торговля** через Bitget Spot v2 API |

### Ключевые параметры `production.yaml`

| Параметр | Описание | Пример |
|----------|----------|--------|
| `trading.initial_capital` | Капитал на аккаунте (USD) | `10.0` |
| `risk.max_position_notional` | Макс. размер позиции | `10.0` |
| `risk.max_daily_loss_pct` | Макс. дневной убыток (%) | `50.0` |
| `risk.kill_switch_enabled` | Аварийный выключатель | `true` |

---

## Архитектура

### Модули (36 подсистем)

| Слой | Модули |
|------|--------|
| **Инфраструктура** | common, config, security, logging, metrics, health, clock, supervisor, platform |
| **Рыночные данные** | exchange/bitget (WS+REST), market_data, normalizer, order_book, buffers, indicators, features |
| **Выбор пар** | pair_scanner (автоматический выбор лучших торговых пар) |
| **Интеллект** | world_model, regime, uncertainty, strategy (×5), strategy_allocator, decision, ai |
| **Исполнение** | execution_alpha, opportunity_cost, portfolio, portfolio_allocator, risk, execution, pipeline |
| **Аналитика** | persistence, replay, telemetry, alpha_decay, shadow, champion_challenger |
| **Защита** | adversarial_defense, synthetic_scenarios, self_diagnosis, governance, operator_control |

### PairScanner — автоматический выбор пар

Модуль `pair_scanner` анализирует все доступные пары на Bitget и выбирает лучшие для торговли:

| Критерий | Описание |
|----------|----------|
| Объём 24ч | Пары с объёмом > 500K USDT (конфигурируемо) |
| Спред | Пары со спредом < 50 bps (конфигурируемо) |
| Волатильность | Оптимальная для краткосрочной торговли |
| ATR | Достаточный диапазон для прибыли |

**Режимы**: `auto` (автоматический скоринг) · `manual` (ручной список) · `hybrid` (авто + белый список)  
**Ротация**: каждые 24 часа (конфигурируемо)  
**Результат**: top-N пар (по умолчанию 5)

### HTF Trend Filter — мультитаймфреймный анализ

Система загружает 200 часовых свечей (8+ дней) и вычисляет тренд на старшем таймфрейме:

| Индикатор | Применение |
|-----------|------------|
| **EMA 20/50** (1h) | Направление тренда: EMA20 > EMA50 → бычий, иначе → медвежий |
| **RSI 14** (1h) | Перепроданность/перекупленность на HTF |
| **MACD** (12/26/9, 1h) | Моментум, сигнал разворота |
| **ADX 14** (1h) | Сила тренда: > 20 = направленный, < 20 = боковик |

**Правила фильтрации**:
- BUY заблокирован при сильном даунтренде (HTF trend = -1, сила > 0.4) без подтверждения разворота
- SELL заблокирован при сильном аптренде (HTF trend = +1, сила > 0.4) без подтверждения разворота
- Разворот подтверждается: MACD histogram меняет знак + RSI выходит из экстремальной зоны

### Market Readiness Gate

Бот **не торгует** до выполнения всех условий:
1. ✅ HTF индикаторы загружены и валидны
2. ✅ Минимум 200 тиков получено (прогрев ~3-5 минут)
3. ✅ RSI не в экстремальной зоне (15-85 на HTF)
4. ✅ Нет сильного даунтренда без признаков разворота

### Торговые стратегии

| Стратегия | Тип | Режим рынка | Защита |
|-----------|-----|-------------|--------|
| **Momentum** | Трендследящая | Trend | ADX > 25, EMA crossover |
| **Mean Reversion** | Возврат к среднему | Ranging | Требует разворот MACD, не покупает в даунтренде |
| **Breakout** | Пробой уровней | Volatile | BB compression → expansion |
| **Microstructure Scalp** | Микроструктурная | Любой (низкий спред) | Не торгует против EMA-тренда |
| **Volatility Expansion** | Расширение волатильности | Volatile | Нейтральный RSI (40-60) |

**Conviction threshold**: 0.35 (взвешенная оценка стратегии должна превысить порог)

### Система защиты от убыточных входов

```
┌─────────────────────────────────────────────┐
│ 1. Market Readiness Gate                     │
│    HTF валидность + прогрев + RSI норма      │
├─────────────────────────────────────────────┤
│ 2. Стратегия (Mean Reversion / Scalp / ...)  │
│    Тренд-фильтр EMA20/50 + MACD reversal    │
├─────────────────────────────────────────────┤
│ 3. Decision Engine (conviction > 0.35)       │
│    Комитетное голосование + вето             │
├─────────────────────────────────────────────┤
│ 4. HTF Trend Filter (финальный барьер)       │
│    BUY блокирован в даунтренде HTF           │
│    SELL блокирован в аптренде HTF            │
├─────────────────────────────────────────────┤
│ 5. Risk Engine (14 проверок)                 │
│    ATR-стоп, макс. убыток, kill switch       │
└─────────────────────────────────────────────┘
```

### Risk Engine — 14 проверок

1. Kill switch
2. Max daily loss
3. Max drawdown
4. Max concurrent positions
5. Max notional exposure
6. Max leverage
7. Max slippage
8. Max order rate
9. Max consecutive losses
10. Stale feed guard
11. Invalid order book guard
12. Spread threshold
13. Liquidity threshold
14. UTC cut-off

---

## Структура проекта

```
tomorrow-bot/
├── CMakeLists.txt              # Корневой CMake
├── README.md                   # Этот файл
├── .env                        # API-ключи (не в VCS)
├── cmake/                      # CMake-модули (FindTALib, Sanitizers)
├── configs/                    # Конфигурации по режимам
│   ├── paper.yaml
│   ├── shadow.yaml
│   ├── testnet.yaml
│   └── production.yaml
├── deploy/                     # Артефакты развёртывания
│   ├── systemd/                # tomorrow-bot.service
│   └── env/                    # Шаблоны .env
├── docs/                       # Документация (30 файлов)
│   ├── architecture/           # Архитектура, потоки данных, FSM
│   ├── guides/                 # Руководства для разработчиков
│   ├── operations/             # Развёртывание, runbooks
│   ├── reference/              # Справка: риск-правила, телеметрия
│   ├── research/               # Схемы feature snapshot
│   ├── runbooks/               # Инцидент-менеджмент
│   └── spec/                   # Спецификации
├── logs/                       # Логи runtime (не в VCS)
├── scripts/                    # Build/run скрипты
├── src/                        # Исходный код (36 модулей, ~20K строк)
│   ├── app/                    # Точка входа, bootstrap, PairScanner интеграция
│   ├── common/                 # StrongType, Result, Error, перечисления
│   ├── config/                 # Загрузка YAML, валидация, хеширование
│   ├── security/               # EnvSecretProvider, маскирование
│   ├── logging/                # Структурированный JSON-логгер
│   ├── metrics/                # Prometheus-совместимые метрики
│   ├── health/                 # Health-check подсистем
│   ├── clock/                  # Абстракция времени (SystemClock / TestClock)
│   ├── supervisor/             # Жизненный цикл, SIGTERM/SIGINT, degraded mode
│   ├── platform/               # Информация о хосте/ОС
│   ├── exchange/bitget/        # WebSocket-клиент + REST-клиент + OrderSubmitter
│   ├── normalizer/             # Нормализация сырых данных Bitget
│   ├── market_data/            # Шлюз рыночных данных, фильтрация heartbeat
│   ├── order_book/             # Локальный L2-стакан с валидацией
│   ├── buffers/                # Lock-free кольцевые буферы
│   ├── indicators/             # TA-Lib обёртки (SMA, EMA, RSI, MACD, BB, ATR, ADX)
│   ├── features/               # FeatureEngine: технические + микроструктурные
│   ├── pair_scanner/           # Автовыбор лучших торговых пар (объём, спред, ATR)
│   ├── world_model/            # 9 состояний мира, fragility, tendency
│   ├── regime/                 # 13 режимов рынка, confidence, stability
│   ├── uncertainty/            # 4-мерная неопределённость
│   ├── strategy/               # 5 стратегий с тренд-фильтрацией
│   ├── strategy_allocator/     # Аллокация по режимам и world suitability
│   ├── decision/               # Комитетное голосование, вето, conviction (порог 0.35)
│   ├── ai/                     # AI Advisory (интерфейс)
│   ├── execution_alpha/        # Passive/Aggressive/Hybrid, urgency, slicing
│   ├── opportunity_cost/       # Ранжирование кандидатов, suppress/defer
│   ├── portfolio/              # Позиции, PnL (realized/unrealized), exposure
│   ├── portfolio_allocator/    # Иерархический: global→regime→strategy→symbol
│   ├── risk/                   # 14 жёстких проверок, kill switch
│   ├── execution/              # FSM (10 состояний), Paper/Bitget submitter
│   ├── pipeline/               # TradingPipeline + HTF Trend Filter + Market Readiness
│   ├── persistence/            # EventJournal + SnapshotStore (IStorageAdapter)
│   ├── replay/                 # ReplayEngine с state machine
│   ├── telemetry/              # Исследовательская телеметрия (envelope)
│   ├── alpha_decay/            # Мониторинг деградации (expectancy, hit-rate, slippage)
│   ├── shadow/                 # Теневые решения, гипотетический PnL
│   ├── champion_challenger/    # A/B тестирование стратегий, promotion
│   ├── self_diagnosis/         # Объяснение решений (human + machine readable)
│   ├── adversarial_defense/    # 6 видов угроз, cooldown, severity
│   ├── governance/             # Audit log (15+ типов событий), strategy registry
│   ├── operator_control/       # 11 команд оператора (kill-switch, shadow, inspect)
│   └── synthetic_scenarios/    # 9 стресс-сценариев с валидацией
├── tests/                      # 203 теста (unit + integration + scenario)
│   ├── unit/                   # Модульные тесты
│   ├── integration/            # Интеграционные тесты
│   ├── mocks/                  # Моки (exchange, persistence)
│   └── data/                   # Тестовые данные
└── tools/                      # Утилиты (replay_inspector, telemetry_viewer)
```

---

## Наблюдаемость

| Канал | Формат | Назначение |
|-------|--------|------------|
| **Логи** | Структурированный JSON | `stdout` или файл (`logs/`) |
| **Метрики** | Prometheus | `http://localhost:9090/metrics` |
| **Health** | JSON | `http://localhost:8080/health` |
| **Телеметрия** | Decision envelope | Полная трассировка каждого решения |

---

## Безопасность

- API-ключи загружаются из `.env` через `EnvSecretProvider`
- Секреты маскируются в логах (`[REDACTED]`)
- Production требует `kill_switch_enabled: true`
- 14 независимых проверок риск-движка перед каждым ордером
- Order FSM исключает дублирование и некорректные переходы
- Graceful shutdown по SIGTERM/SIGINT с сохранением состояния
- Systemd service с `PrivateTmp`, `ProtectSystem=strict`, `NoNewPrivileges`

---

## Документация

| Раздел | Описание |
|--------|----------|
| [Архитектурный обзор](docs/architecture/overview.md) | Модули, слои, принципы |
| [Потоки данных](docs/architecture/data_flow.md) | WebSocket → PairScanner → HTF → Pipeline |
| [Потоки решений](docs/architecture/decision_flow.md) | Strategy → HTF Filter → Execution |
| [FSM ордеров](docs/architecture/order_fsm.md) | 10 состояний, переходы |
| [Руководство по конфигурации](docs/guides/config_guide.md) | Параметры YAML, pair_selection |
| [Расширение стратегий](docs/guides/strategy_extension_guide.md) | Как добавить стратегию |
| [Shadow Mode](docs/guides/shadow_mode_guide.md) | Теневая торговля |
| [Champion-Challenger](docs/guides/champion_challenger_guide.md) | A/B тестирование |
| [Развёртывание](docs/operations/production_deployment_guide.md) | Production setup |
| [Operations runbook](docs/operations/operations_runbook.md) | Эксплуатация |
| [Правила риска](docs/reference/risk_rules.md) | 14 проверок |
| [Kill Switch](docs/runbooks/kill_switch_runbook.md) | Аварийная остановка |
| [Известные ограничения](docs/architecture/known_limitations.md) | Roadmap |

---

## Лицензия

Проприетарное ПО. Все права защищены.
