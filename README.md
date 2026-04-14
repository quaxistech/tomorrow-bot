# Tomorrow Bot

Адаптивная алгоритмическая торговая система для фьючерсов USDT-M на бирже Bitget.
C++23, скальпинг-стратегия, полный цикл от сканирования пар до исполнения ордеров.

**v0.5.0** · **35 модулей** · **222 файла** · **46 000+ строк** · **556 тестов**

---

## Архитектура

```
┌──────────────┐
│ ScannerEngine│ Bitget REST → top-N пар → ротация каждые 24ч
└──────┬───────┘
       │  для каждого символа создаётся TradingPipeline
       ▼
┌──────────────────────────────────────────────────────────────┐
│  TradingPipeline                                             │
│                                                              │
│  WebSocket → Normalizer → OrderBook → FeatureEngine          │
│                                          │                   │
│              ┌───────────────────────────┬┘                   │
│              ▼                           ▼                    │
│         WorldModel + RegimeEngine   UncertaintyEngine         │
│              │            │              │                    │
│              └─────┬──────┘              │                    │
│                    ▼                     │                    │
│           StrategyAllocator ◀────────────┘                   │
│                    │                                         │
│           StrategyEngine (4 скальп-сценария)                 │
│                    │                                         │
│           DecisionEngine (conviction + конфликты)            │
│                    │                                         │
│           ML Layer (6 компонентов)                           │
│                    │                                         │
│        ┌───────────┼────────────┐                            │
│        ▼           ▼            ▼                            │
│   ExecAlpha   OpportCost   PortfolioAllocator                │
│        └───────────┼────────────┘                            │
│                    ▼                                         │
│           LeverageEngine (1×–20×)                            │
│                    ▼                                         │
│           RiskEngine (33 проверки)                           │
│                    ▼                                         │
│           ExecutionEngine → Bitget REST API                  │
│                    ▼                                         │
│           Reconciliation ◀──▶ Portfolio                      │
└──────────────────────────────────────────────────────────────┘
       │
       ▼
  Supervisor (жизненный цикл, сигналы, kill switch)
```

---

## Быстрый старт

### Зависимости

| Компонент | Версия |
|-----------|--------|
| GCC | 14+ (C++23) |
| CMake | 3.25+ |
| Ninja | любая |
| Boost | 1.82+ (system, json, thread) |
| OpenSSL | 3.x |
| libpqxx + PostgreSQL | для персистенции |
| Catch2 | v3 (FetchContent) |

```bash
# Ubuntu 24.04
sudo apt install g++-14 cmake ninja-build \
  libboost-all-dev libssl-dev libpqxx-dev \
  postgresql-server-dev-all
```

### Сборка

```bash
# Debug (с санитайзерами)
./scripts/build_debug.sh

# Release
./scripts/build_release.sh

# Или вручную
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build -j$(nproc)
```

### Тесты

```bash
cd build && ctest -j$(nproc) --output-on-failure
```

### Запуск

```bash
# Бумажная торговля (production.yaml с mode: paper)
./build/src/app/tomorrow-bot --config configs/production.yaml

# Production (требует secrets и подтверждение)
./run_prod.sh
# или
./scripts/run_production.sh
```

API-ключи Bitget — через файл `.env` в корне проекта:

```env
BITGET_API_KEY=...
BITGET_API_SECRET=...
BITGET_PASSPHRASE=...
```

---

## Режимы

| Режим | Конфиг | Описание |
|-------|--------|----------|
| Paper | `configs/production.yaml` (mode: paper) | Симуляция, ордера не отправляются |
| Production | `configs/production.yaml` | Реальная торговля USDT-M фьючерсами |

Production-режим требует переменную окружения:
```bash
export TOMORROW_BOT_PRODUCTION_CONFIRM=I_UNDERSTAND_LIVE_TRADING
```

---

## Структура проекта

```
tomorrow-bot/
├── src/                        # 35 модулей
│   ├── app/                    # Точка входа, bootstrap
│   ├── common/                 # Типы, Result, ошибки
│   ├── config/                 # Загрузка YAML, валидация
│   ├── security/               # EnvSecretProvider, маскирование
│   ├── logging/                # Структурированный JSON-логгер
│   ├── metrics/                # Prometheus-метрики
│   ├── clock/                  # IClock (system / test)
│   ├── supervisor/             # Жизненный цикл, сигналы, symbol locks
│   ├── exchange/bitget/        # REST + WS + Futures submitter
│   ├── normalizer/             # Нормализация данных Bitget
│   ├── market_data/            # WebSocket-шлюз
│   ├── order_book/             # Локальный L2-стакан
│   ├── buffers/                # Кольцевые буферы
│   ├── indicators/             # RSI, EMA, MACD, ATR, ADX, BB, OBV, VWAP...
│   ├── features/               # FeatureEngine + AdvancedFeatures
│   ├── world_model/            # 9 состояний мира
│   ├── regime/                 # 13 режимов рынка
│   ├── uncertainty/            # 9-мерная неопределённость
│   ├── strategy/               # Скальпинг (4 сценария)
│   ├── strategy_allocator/     # Аллокация весов по режимам
│   ├── decision/               # Комитетное голосование
│   ├── ml/                     # 6 ML-компонентов
│   ├── leverage/               # Адаптивное плечо 1×–20×
│   ├── execution_alpha/        # Passive/Aggressive/Hybrid
│   ├── opportunity_cost/       # Ранжирование кандидатов
│   ├── portfolio/              # Позиции, PnL, exposure
│   ├── portfolio_allocator/    # Иерархический сайзинг
│   ├── risk/                   # 33 policy-проверки
│   ├── execution/              # FSM ордеров, TWAP
│   ├── scanner/                # Выбор торговых пар
│   ├── reconciliation/         # Сверка с биржей
│   ├── recovery/               # Snapshot + WAL restore
│   ├── resilience/             # Circuit breaker, retry, idempotency
│   └── persistence/            # PostgreSQL (journal, snapshots)
├── tests/                      # 556 тестов (Catch2 v3)
│   ├── unit/                   # 27 модулей юнит-тестов
│   ├── integration/            # Интеграционные тесты
│   ├── common/                 # Тестовые моки
│   └── data/                   # Тестовые данные
├── configs/                    # YAML-конфигурации
├── scripts/                    # Скрипты сборки и запуска
├── deploy/                     # systemd, env-шаблоны
├── tools/                      # config_validator, log_summarizer,
│                               # replay_inspector, telemetry_viewer
└── audit.md                    # Аудит системы
```

---

## Документация

Основная документация содержится в `audit.md` и комментариях к коду.
Конфигурация описана в `configs/production.yaml`.

---

## Ключевые характеристики

- **Bitget USDT-M Futures** — hedge mode, isolated margin
- **Скальпинг** — 4 сценария (momentum, retest, pullback, rejection), state machine
- **Адаптивное плечо** — 1×–20× (режим, волатильность, просадка, фандинг)
- **ScannerEngine** — автоматический top-N пар, ротация, trap-детекция
- **33 проверки риска** — kill switch, лимиты просадки, cooldown, regime-scaled
- **6 ML-компонентов** — Bayesian, Thompson, entropy, fingerprint, cascade, correlation
- **Reconciliation** — автоматическая сверка портфеля с биржей при рестарте
- **Recovery** — snapshot + WAL replay + exchange sync
- **Resilience** — circuit breaker, retry с backoff, idempotency
- **Метрики** — Prometheus-совместимый экспортёр

---

## Лицензия

Проприетарное ПО. Все права защищены.
