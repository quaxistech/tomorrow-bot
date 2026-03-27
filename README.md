# Tomorrow Bot — Adaptive Trading System

Производственная адаптивная торговая операционная система для биржи Bitget.
Написана на C++23, развёртывается на Ubuntu 24.04.

**Версия**: 0.3.0 | **Статус**: Production | **Тесты**: 407 тестов

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
    AdvancedFeatureEngine (CUSUM, VPIN, VolumeProfile, ToD)     │
                         │                                      │
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
                       ▼
              ML Layer (Entropy Filter, Fingerprint, Cascade, Correlation)
                       │
                       ▼
              Bayesian Adaptation · Thompson Sampling Entry Timing
                       │
                    ┌──┴────────┬───────────┐
                    ▼           ▼           ▼
            ExecutionAlpha  OpportunityCost  PortfolioAllocator
                    │           │           │
                    └───────────┴─────┬─────┘
                                      ▼
                        Chandelier Exit / Trailing Stop
                                      │
                                      ▼
                        Volatility Targeting (сайзинг)
                                      │
                                      ▼
                                RiskEngine (14 проверок)
                                      │
                                      ▼
                    ExecutionEngine → TWAP Executor → Bitget REST API
                                      │
                                      ▼
                        Alpha Decay Feedback → PortfolioEngine (PnL)
```

**43+ модулей** · **9 стратегий** · **14 проверок риска** · **10 состояний FSM ордера** · **HTF тренд-фильтр** · **6 ML-модулей** · **Smart TWAP** · **Trailing Stop**

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
ctest --output-on-failure -j8   # 407 тестов
```

### Конфигурация API-ключей

Создайте файл `.env` в корне проекта:

```bash
BITGET_API_KEY=ваш_api_key
BITGET_API_SECRET=ваш_secret_key
BITGET_PASSPHRASE=ваш_passphrase
```

Бот автоматически загружает `.env` при запуске. Файл защищён `.gitignore`.

### Настройка PostgreSQL (для Alpha Decay Persistence)

Alpha Decay Monitor использует PostgreSQL для персистентности истории сделок и восстановления
после перезапуска. Если `POSTGRES_URL` не задан — модуль работает только в памяти.

```bash
# Установка PostgreSQL (если не установлен)
sudo apt install -y postgresql libpqxx-dev

# Создание базы данных для бота
sudo -u postgres psql -c "CREATE USER tbbot WITH PASSWORD 'your_password';"
sudo -u postgres psql -c "CREATE DATABASE tomorrow_bot OWNER tbbot;"

# Добавьте в .env:
POSTGRES_URL=host=localhost dbname=tomorrow_bot user=tbbot password=your_password

# Таблицы создаются автоматически при первом запуске:
#   tb_journal   — append-only журнал сигналов и сделок
#   tb_snapshots — снапшоты состояния стратегий
```

Схема таблиц (DDL создаётся автоматически):

```sql
-- Журнал событий (индексирован по типу и strategy_id)
CREATE TABLE IF NOT EXISTS tb_journal (
    id          BIGSERIAL PRIMARY KEY,
    ts_ns       BIGINT NOT NULL,
    entry_type  INT NOT NULL,
    strategy_id TEXT NOT NULL DEFAULT '',
    symbol      TEXT NOT NULL DEFAULT '',
    payload_json TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_tj_type_strat ON tb_journal(entry_type, strategy_id);
CREATE INDEX IF NOT EXISTS idx_tj_ts ON tb_journal(ts_ns);

-- Снапшоты (последнее значение по ключу)
CREATE TABLE IF NOT EXISTS tb_snapshots (
    key         TEXT PRIMARY KEY,
    ts_ns       BIGINT NOT NULL,
    payload_json TEXT NOT NULL
);
```

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

### Модули (42+ подсистем)

| Слой | Модули |
|------|--------|
| **Инфраструктура** | common, config, security, logging, metrics, health, clock, supervisor, platform |
| **Рыночные данные** | exchange/bitget (WS+REST), market_data, normalizer, order_book, buffers, indicators, features |
| **Выбор пар** | pair_scanner (автоматический выбор лучших торговых пар) |
| **Продвинутые признаки** | advanced_features (CUSUM, VPIN, Volume Profile, Time-of-Day Alpha) |
| **Интеллект** | world_model, regime, uncertainty, strategy (×5), strategy_allocator, decision, ai |
| **ML/AI** | bayesian_adapter, entropy_filter, microstructure_fingerprint, liquidation_cascade, correlation_monitor, thompson_sampler |
| **Исполнение** | execution_alpha, opportunity_cost, portfolio, portfolio_allocator, risk, execution, pipeline, twap_executor |
| **Аналитика** | persistence, replay (event-driven replay + backtest engine + fill simulator + performance metrics), telemetry, alpha_decay, shadow (v2.0: виртуальное исполнение, fill simulation, multi-leg positions, configurable eval windows, Prometheus metrics, PostgreSQL persistence), champion_challenger |
| **Защита** | adversarial_defense (v4: 14 детекторов, percentile scoring, correlation matrix, multi-TF, hysteresis, audit log, calibration), synthetic_scenarios, self_diagnosis (v2: event-sourced, scorecards, corrective actions, pipeline-integrated), governance (runtime control plane), operator_control |

### Продвинутые технологии v0.3

#### Уровень 1 — Управление рисками

| Технология | Описание |
|-----------|----------|
| **Chandelier Exit / Trailing Stop** | Адаптивный trailing stop: 3×ATR (сильный тренд ADX>30), 2.5× (умеренный), 1.5× (choppy). Breakeven при +1.5×ATR прибыли. Partial TP 50% при +2×ATR. Стоп только подтягивается, никогда не откатывается |
| **Volatility Targeting** | Динамический сайзинг: size = target_vol / realized_vol × Kelly × regime_mult. 13 режимов с множителями 0.1–1.0. Kelly fraction = 0.25 (консервативно) |
| **Alpha Decay Feedback** | Профессиональный мониторинг деградации по 7 измерениям: Expectancy, HitRate, SlippageAdjusted, ExecutionQuality, RegimeConditioned, ConfidenceReliability (Brier score), AdverseExcursion (MAE). Per-strategy множители размера/порогов. Гистерезис (stable_count≥2). Экспорт метрик в Prometheus. Персистентность истории в PostgreSQL. Рекомендации: ReduceWeight(×0.7), ReduceSize(×0.5, +0.05 threshold), RaiseThresholds(+0.10), Disable(×0.0). Проверка каждые 60с |
| **HTF Real-Time Update** | Пересчёт HTF индикаторов каждый час через REST + экстренное обновление при движении > 3×ATR |

#### Уровень 2 — Продвинутые индикаторы

| Технология | Описание |
|-----------|----------|
| **CUSUM** | Two-sided cumulative sum для раннего обнаружения смены режима. Drift 0.5σ, threshold 4σ. Sample stddev (Bessel's correction). Без look-ahead bias |
| **VPIN** | Volume-Synchronized Probability of Informed Trading. Volume buckets, порог токсичности 0.7. 10-trade calibration для надёжности |
| **Volume Profile** | POC (Point of Control) + Value Area (70%). 50 ценовых уровней, 5000 трейдов lookback. Пересчёт каждые 100 трейдов |
| **Smart TWAP** | Адаптивное разбиение крупных ордеров на 3-10 слайсов. Интервал адаптируется к спреду и VPIN. Front-loading 1.2× |
| **Time-of-Day Alpha** | 24-часовой UTC профиль: множители волатильности и альфа-скоры по сессиям (азиатская, европейская, американская). +0.05 к порогу в тихие часы |

#### Уровень 3 — ML/AI

| Технология | Описание |
|-----------|----------|
| **Bayesian Online Adaptation** | Normal-Normal conjugate prior для онлайн обновления параметров стратегий. Regime-conditioned (70% вес текущего режима). Thompson Sampling exploration (10%) |
| **Entropy Filter** | Shannon entropy на 4 каналах (returns, volume, spread, flow). Блокирует торговлю при entropy > 0.85 (шум). Результат кешируется |
| **Microstructure Fingerprinting** | 5D fingerprint (spread, imbalance, flow, volatility, depth) × 5 buckets = 3125 patterns. Knowledge base с win_rate. Блокирует вход при edge < -0.1 |
| **Liquidation Cascade Prediction** | Детектор каскадных ликвидаций: velocity 40%, volume spike 30%, depth thinning 30%. Порог 0.6 |
| **Correlation Monitor** | Pearson correlation (short 20 / long 100 window) с BTC и ETH. Decorrelation → risk_mult 0.5 |
| **RL Entry Timing (Thompson Sampling)** | 5-arm Beta bandit (EnterNow, Wait1/2/3, Skip). Exponential decay 0.995 для non-stationarity |

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
| **Momentum** | Трендследящая | Trend | ADX > 25, EMA crossover, volatility-scaled thresholds |
| **Mean Reversion** | Возврат к среднему | Ranging | Требует разворот MACD, не покупает в даунтренде |
| **Breakout** | Пробой уровней | Volatile | BB compression → expansion, volume confirmation |
| **Microstructure Scalp** | Микроструктурная | Любой (низкий спред) | Не торгует против EMA-тренда, VPIN gating |
| **Volatility Expansion** | Расширение волатильности | Volatile | Нейтральный RSI (40-60), consistent ATR windows |
| **EMA Pullback** | Откат к тренду | Trending | Вход после отката к EMA20 в направлении тренда, ADX confirm |
| **RSI Divergence** | Дивергенция | Ranging/Volatile | Бычья/медвежья дивергенция RSI vs цена, MACD confirm |
| **VWAP Reversion** | Возврат к VWAP | Ranging | Отклонение от trade VWAP, volume + book imbalance confirm |
| **Volume Profile** | Профиль объёма | Ranging | Сделки от POC/VA уровней, BB + momentum confirmation |

Все стратегии поддерживают:
- **Typed config** — конфигурируемые пороги (не хардкод)
- **SignalIntent** — явная спотовая семантика (LongEntry/LongExit/ReducePosition/Hold)
- **ExitReason** — причина выхода для аналитики (TakeProfit, TrendFailure, RangeTopExit, etc.)

**Conviction threshold**: 0.35 (взвешенная оценка стратегии должна превысить порог)

### Система защиты от убыточных входов

```
┌─────────────────────────────────────────────┐
│ 1. Market Readiness Gate                     │
│    HTF валидность + прогрев + RSI норма      │
├─────────────────────────────────────────────┤
│ 2. Governance Gate (runtime control plane)   │
│    Kill switch · HaltMode · IncidentState    │
│    Strategy lifecycle · Safe mode             │
├─────────────────────────────────────────────┤
│ 3. Entropy Filter (ML quality gate)          │
│    Shannon entropy < 0.85 на 4 каналах       │
├─────────────────────────────────────────────┤
│ 4. Стратегия (Mean Reversion / Scalp / ...)  │
│    Тренд-фильтр EMA20/50 + MACD reversal    │
├─────────────────────────────────────────────┤
│ 5. Fingerprint Check                         │
│    Микроструктурный паттерн edge > -0.1      │
├─────────────────────────────────────────────┤
│ 6. Cascade Check (аварийная блокировка)      │
│    Ликвидационный каскад score < 0.6         │
├─────────────────────────────────────────────┤
│ 7. Correlation Risk Adjustment               │
│    Decorrelation с BTC/ETH → risk_mult 0.5   │
├─────────────────────────────────────────────┤
│ 8. Bayesian Adaptation + Thompson Sampling   │
│    Онлайн обновление параметров + entry timing│
├─────────────────────────────────────────────┤
│ 9. Decision Engine (conviction > dynamic)    │
│    Порог = base 0.35 + alpha_decay + tod_adj │
├─────────────────────────────────────────────┤
│ 10. HTF Trend Filter (финальный барьер)      │
│    BUY блокирован в даунтренде HTF           │
│    SELL блокирован в аптренде HTF            │
├─────────────────────────────────────────────┤
│ 11. Volatility Targeting (сайзинг)           │
│    size = target_vol / realized_vol × Kelly   │
├─────────────────────────────────────────────┤
│ 12. Risk Engine (14 проверок)                │
│    ATR-стоп, макс. убыток, kill switch       │
├─────────────────────────────────────────────┤
│ 13. Chandelier Exit / Trailing Stop          │
│    Адаптивный ATR-стоп (1.5×–3×)             │
├─────────────────────────────────────────────┤
│ 14. Smart TWAP (исполнение крупных ордеров)  │
│    3-10 слайсов, адаптивный интервал         │
├─────────────────────────────────────────────┤
│ 15. Alpha Decay Feedback                     │
│    Деградация → ReduceWeight/Disable         │
├─────────────────────────────────────────────┤
│ 16. Adversarial Defense v4                   │
│    14 детекторов, percentile scoring,        │
│    correlation matrix, multi-TF baselines,   │
│    hysteresis, regime-aware порог, audit log  │
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

### Governance Control Plane

Governance — централизованный runtime control plane системы. Каждый торговый тик проходит
через governance gate перед стратегиями. Все изменения состояния записываются в durable audit trail.

#### Архитектура

```
                    AppBootstrap
                        │
                        ▼
              GovernanceAuditLayer ─────────── EventJournal (durable)
                   │    │    │
          ┌────────┤    │    ├────────┐
          ▼        ▼    ▼    ▼        ▼
    Supervisor  Pipeline  RiskEngine  OperatorControl
                   │
              Governance Gate
         (kill switch / halt mode /
          incident / strategy lifecycle)
                   │
                   ▼
           ... trading continues ...
```

#### Governance Gate — 6-уровневая проверка

| # | Проверка | Описание |
|---|----------|----------|
| 1 | Kill Switch | Полная блокировка торговли |
| 2 | Halt Mode | `HardHalt` · `CloseOnly` · `ReduceOnly` · `NoNewEntries` · `None` |
| 3 | Incident State | `Halted` → блокировка; `Restricted` → close-only; `Degraded` → no new entries |
| 4 | Safe Mode | Только reduce/close при активном safe mode |
| 5 | Strategy Enabled | Стратегия должна быть enabled в реестре |
| 6 | Strategy Lifecycle | Только `Live` и `Shadow` разрешены для торговли |

#### Halt Modes

| Режим | Описание |
|-------|----------|
| `None` | Нет ограничений |
| `NoNewEntries` | Запрет новых позиций, закрытие разрешено |
| `ReduceOnly` | Только уменьшение позиций |
| `CloseOnly` | Только закрытие позиций |
| `HardHalt` | Полная остановка торговли |

#### Incident State Machine

```
Normal ←→ Degraded → Restricted → Halted
  ↑                                  │
  └───────── Recovering ←────────────┘
```

| Состояние | Описание | Торговля |
|-----------|----------|----------|
| `Normal` | Нормальная работа | Полная |
| `Degraded` | Частичные сбои | Только close/reduce |
| `Restricted` | Серьёзные проблемы | Close-only |
| `Halted` | Полная остановка | Заблокирована |
| `Recovering` | Восстановление | Заблокирована |

#### Strategy Lifecycle

```
Registered → Shadow → Candidate → Live → Draining → Disabled → Retired
```

| Состояние | Описание | Торговля |
|-----------|----------|----------|
| `Registered` | Зарегистрирована, не запущена | Нет |
| `Shadow` | Теневой режим (наблюдение) | Да (shadow) |
| `Candidate` | Кандидат на promotion | Нет |
| `Live` | Боевой режим | Да |
| `Draining` | Закрытие позиций | Нет |
| `Disabled` | Выключена | Нет |
| `Retired` | Окончательно выведена | Нет |

#### Единый Kill Switch

Risk Engine делегирует kill switch в governance (единый source of truth).
При активации risk engine или оператором — kill switch фиксируется
в governance, записывается в durable audit, обновляются метрики и health.

#### Durable Audit Trail

23 типа аудиторских событий записываются в:
- **In-memory ring buffer** (10 000 записей) — для быстрого доступа
- **EventJournal** (PostgreSQL) — для forensic analysis и восстановления

Каждая запись содержит: `audit_id`, `type`, `timestamp`, `actor`, `target`,
`details`, `config_hash`, `subsystem`, `severity`, `previous_state`, `new_state`.

#### Метрики Governance (Prometheus)

| Метрика | Тип | Описание |
|---------|-----|----------|
| `governance_kill_switch_active` | Gauge | 1 = active, 0 = inactive |
| `governance_safe_mode_active` | Gauge | 1 = active, 0 = inactive |
| `governance_halt_mode` | Gauge | 0-4 (None → HardHalt) |
| `governance_incident_state` | Gauge | 0-4 (Normal → Recovering) |
| `governance_strategies_total` | Gauge | Количество зарегистрированных стратегий |
| `governance_audit_events_total` | Counter | Общее число аудиторских событий |

### Self-Diagnosis Engine v2

Модуль самодиагностики превращён из локального explain-модуля в production subsystem, интегрированную в торговый pipeline.

#### Архитектура

```
TradingPipeline
  ├─ decision denied    → explain_denial()
  ├─ risk denied        → explain_denial()
  ├─ order executed     → explain_trade()
  ├─ order failed       → diagnose_execution_failure()
  └─ periodic (500 tick)→ diagnose_system_state()

SelfDiagnosisEngine
  ├─ 12 типов DiagnosticType
  ├─ 4 уровня DiagnosticSeverity (Info → Fatal)
  ├─ 7 CorrectiveAction (Observe → HaltSystem)
  ├─ Event-sourced: каждая запись → EventJournal
  ├─ Scorecards: агрегация по стратегиям
  └─ Метрики Prometheus: tb_diag_records_total, tb_diag_max_severity
```

#### Типы диагностических событий

| Тип | Описание |
|-----|----------|
| `TradeTaken` | Объяснение совершённой сделки |
| `TradeDenied` | Объяснение отказа от сделки |
| `SystemState` | Диагностика состояния системы |
| `DegradedState` | Деградированное состояние |
| `ExecutionFailure` | Ошибка исполнения ордера |
| `MarketDataDegradation` | Деградация рыночных данных |
| `ExchangeConnectivityIncident` | Инцидент подключения к бирже |
| `StrategySuppression` | Подавление стратегии (alpha decay) |
| `PortfolioConstraint` | Ограничение портфеля |
| `ReconciliationMismatch` | Расхождение при сверке с биржей |
| `RecoveryAction` | Действие восстановления |
| `StrategyHealth` | Здоровье стратегии |

#### Корректирующие действия

| Действие | Описание | Автоматический триггер |
|----------|----------|------------------------|
| `Observe` | Только наблюдать | TradeTaken, StrategyHealth |
| `SlowDown` | Увеличить интервалы | Единичные execution failures |
| `ReduceSize` | Уменьшить размер позиций | DegradedState, PortfolioConstraint |
| `StopEntries` | Запретить новые входы | Множественные execution failures |
| `ForceReconcile` | Принудительная сверка | ReconciliationMismatch |
| `HaltSymbol` | Остановить торговлю символом | MarketDataDegradation (critical) |
| `HaltSystem` | Остановить всю торговлю | ExchangeConnectivity (critical) |

#### Strategy Scorecards

Агрегированная статистика по каждой стратегии:
- `trades_taken` / `trades_denied` / `execution_failures` / `suppressions`
- `denial_rate` — доля отклонённых сигналов
- `avg_conviction` — средняя убеждённость сигналов
- Сброс при дневной ротации (`reset_scorecards()`)

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
├── src/                        # Исходный код (42+ модулей, ~25K строк)
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
│   ├── features/               # FeatureEngine + AdvancedFeatures (CUSUM, VPIN, VolumeProfile, ToD)
│   ├── pair_scanner/           # Автовыбор лучших торговых пар (объём, спред, ATR)
│   ├── world_model/            # 9 состояний мира, fragility, tendency
│   ├── regime/                 # 13 режимов рынка, confidence, stability
│   ├── uncertainty/            # 4-мерная неопределённость
│   ├── strategy/               # 9 стратегий с тренд-фильтрацией
│   ├── strategy_allocator/     # Аллокация по режимам и world suitability
│   ├── decision/               # Комитетное голосование, вето, conviction (порог 0.35)
│   ├── ai/                     # AI Advisory (интерфейс)
│   ├── ml/                     # ML-модули (6 модулей, 12 файлов)
│   │   ├── bayesian_adapter    # Bayesian Online Adaptation (Normal-Normal conjugate)
│   │   ├── entropy_filter      # Shannon entropy на 4 каналах, quality gate
│   │   ├── microstructure_fingerprint  # 5D fingerprint, 3125 паттернов
│   │   ├── liquidation_cascade # Детектор каскадных ликвидаций
│   │   ├── correlation_monitor # Pearson correlation с BTC/ETH
│   │   └── thompson_sampler    # 5-arm Beta bandit, RL entry timing
│   ├── execution_alpha/        # Passive/Aggressive/Hybrid, urgency, slicing
│   ├── opportunity_cost/       # Ранжирование кандидатов, suppress/defer
│   ├── portfolio/              # Позиции, PnL (realized/unrealized), exposure
│   ├── portfolio_allocator/    # Иерархический: global→regime→strategy→symbol
│   ├── risk/                   # 14 жёстких проверок, kill switch
│   ├── execution/              # FSM (10 состояний), Paper/Bitget submitter, TWAP executor
│   ├── pipeline/               # TradingPipeline + HTF Trend Filter + Market Readiness
│   ├── persistence/            # EventJournal + SnapshotStore (IStorageAdapter)
│   ├── replay/                 # ReplayEngine (pause/resume/seek, 4 режима, hooks) + BacktestEngine (fill simulation, equity curve, Sharpe/Sortino/Calmar)
│   ├── telemetry/              # Исследовательская телеметрия (envelope)
│   ├── alpha_decay/            # Мониторинг деградации (7 измерений: expectancy, hit-rate, slippage, MAE, Brier, regime, execution quality). PostgreSQL persistence
│   ├── shadow/                 # Shadow v2.0: виртуальное исполнение, fill simulation, multi-leg positions, P&L tracking, divergence alerts, Prometheus metrics, PostgreSQL persistence
│   ├── champion_challenger/    # A/B тестирование стратегий, promotion
│   ├── self_diagnosis/         # Самодиагностика v2: 12 типов событий, severity/corrective actions, scorecards, event-sourced persistence, pipeline-integrated
│   ├── adversarial_defense/    # 14 детекторов, v4: percentile, correlation, MTF, hysteresis
│   ├── governance/             # Runtime control plane: governance gate, halt modes, incident FSM, strategy lifecycle, durable audit (23 event types)
│   ├── operator_control/       # 11 команд оператора (kill-switch, shadow, inspect)
│   └── synthetic_scenarios/    # 9 стресс-сценариев с валидацией
├── tests/                      # 433 теста (unit + integration + scenario)
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
- Governance control plane — единый kill switch, halt modes, durable audit trail
- 14 независимых проверок риск-движка перед каждым ордером
- Chandelier Exit / Trailing Stop — адаптивный стоп (1.5×–3×ATR)
- Adversarial Defense v4 — 14 детекторов рыночных угроз с автоматической калибровкой
- Entropy Filter — блокировка торговли при высоком шуме (entropy > 0.85)
- Liquidation Cascade Detector — аварийная блокировка при каскадных ликвидациях
- Correlation Monitor — снижение риска при decorrelation с BTC/ETH
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
| [Shadow Mode v2.0](docs/guides/shadow_mode_guide.md) | Виртуальное исполнение: fill simulation, position tracking, P&L, алерты |
| [Champion-Challenger](docs/guides/champion_challenger_guide.md) | A/B тестирование |
| [Развёртывание](docs/operations/production_deployment_guide.md) | Production setup |
| [Operations runbook](docs/operations/operations_runbook.md) | Эксплуатация |
| [Правила риска](docs/reference/risk_rules.md) | 14 проверок |
| [Kill Switch](docs/runbooks/kill_switch_runbook.md) | Аварийная остановка |
| [Governance Control Plane](docs/guides/governance_guide.md) | Runtime control plane, governance gate, halt modes, incident FSM |
| [Известные ограничения](docs/architecture/known_limitations.md) | Roadmap |

---

## Лицензия

Проприетарное ПО. Все права защищены.
