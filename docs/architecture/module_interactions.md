# Взаимодействие модулей системы Tomorrow Bot

## Полный граф зависимостей

### Инфраструктура

```
main.cpp
  └─→ AppBootstrap
        ├─→ IConfigLoader (YamlConfigLoader)
        │     └─→ ConfigValidator
        │           └─→ ConfigHash (OpenSSL SHA-256)
        ├─→ ISecretProvider (EnvSecretProvider)
        ├─→ ILogger (ConsoleLogger + JsonFormatter)
        ├─→ IMetricsRegistry (InMemoryMetricsRegistry)
        ├─→ IHealthService (HealthService)
        ├─→ IClock (WallClock)
        └─→ Supervisor (жизненный цикл, SIGTERM/SIGINT)
```

### Торговый Pipeline (полный поток данных)

```
Bitget WebSocket (ticker, trade, books, candle1m, candle5m)
  │
  ▼
MarketDataGateway (фильтрация heartbeat/subscribe)
  │
  ▼
Normalizer (JSON → типизированные события)
  │
  ├──► OrderBook (L2 стакан, валидация, quality tracking)
  ├──► CandleBuffer (кольцевой буфер свечей)
  └──► TradeBuffer (кольцевой буфер сделок)
        │
        ▼
  FeatureEngine (технические + микроструктурные фичи)
  │  ├── SMA, EMA, MACD, RSI, Bollinger, ATR, ADX (TA-Lib)
  │  └── spread, depth_imbalance, book_instability
  │
  ├──► WorldModel (9 состояний: StableTrend, FragileBreakout, ChopNoise...)
  ├──► RegimeEngine (13 режимов: StrongUptrend, MeanReversion, ToxicFlow...)
  └──► UncertaintyEngine (4 измерения: regime, signal, data, execution)
        │
        ▼
  StrategyAllocator (режим → веса стратегий)
  │
  ├──► MomentumStrategy (trend-following)
  ├──► MeanReversionStrategy (return-to-mean)
  ├──► BreakoutStrategy (level breakout)
  ├──► MicrostructureScalpStrategy (microstructure)
  └──► VolatilityExpansionStrategy (vol expansion)
        │
        ▼
  DecisionAggregationEngine (комитетное голосование, вето)
  │
  ├──► ExecutionAlphaEngine (passive/aggressive/hybrid, urgency, slice plan)
  ├──► OpportunityCostEngine (ранжирование, suppress/defer)
  └──► PortfolioAllocator (global → regime → strategy → symbol бюджет)
        │
        ▼
  RiskEngine (14 жёстких проверок — ОБЯЗАТЕЛЬНАЯ ПРОВЕРКА)
  │  ├── kill_switch, max_daily_loss, max_drawdown
  │  ├── max_positions, max_exposure, max_leverage
  │  ├── stale_feed, invalid_book, spread, liquidity
  │  └── max_slippage, max_order_rate, max_consecutive_losses
  │
  ▼
  ExecutionEngine (Order FSM: 10 состояний)
  │  ├── IOrderSubmitter (interface)
  │  ├── PaperOrderSubmitter (paper/shadow mode)
  │  └── BitgetOrderSubmitter → BitgetRestClient → Bitget REST API
  │
  ▼
  PortfolioEngine (positions, PnL, exposure)
```

### Аналитика и защита (параллельно pipeline)

```
TradingPipeline
  ├──► Persistence (EventJournal + SnapshotStore)
  ├──► Telemetry (decision envelope)
  ├──► SelfDiagnosis (объяснение решений)
  ├──► Shadow (теневые решения, гипотетический PnL)
  ├──► ChampionChallenger (A/B тестирование)
  ├──► AlphaDecay (мониторинг деградации)
  ├──► AdversarialDefense (6 видов угроз)
  ├──► Governance (audit log, strategy registry)
  └──► OperatorControl (11 команд оператора)
```

## Переключение Paper ↔ Production

Единственное отличие — тип `IOrderSubmitter`:

```
config.trading.mode == Production/Testnet
  → BitgetOrderSubmitter (реальные HTTP-запросы)

config.trading.mode == Paper/Shadow
  → PaperOrderSubmitter (мгновенное подтверждение в памяти)
```

Выбор происходит в `TradingPipeline` конструкторе на основе конфигурации.
Все остальные модули работают идентично в обоих режимах.
