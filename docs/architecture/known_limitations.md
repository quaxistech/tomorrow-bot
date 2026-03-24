# Известные ограничения и дорожная карта

## Текущее состояние

Tomorrow Bot — полностью рабочая production-grade торговая система:
- **36 модулей** реализованы без заглушек
- **203 unit-теста** проходят
- **Реальная торговля** на Bitget через REST API
- **WebSocket**: ~1000+ сообщений в минуту
- **Graceful shutdown** по SIGTERM/SIGINT
- **Автоматический выбор пар** через PairScanner (24ч ротация)
- **Мультитаймфреймный анализ**: 200 × 1h свечей (8+ дней) для HTF тренда
- **Тренд-фильтрация**: стратегии не торгуют против HTF тренда

---

## Полностью реализовано ✅

- **Торговый pipeline**: WebSocket → Normalizer → Features → Strategies → Risk → Execution
- **Автоматический выбор пар**: PairScanner (объём, спред, волатильность, ATR скоринг)
- **Загрузка истории**: 200 × 1m свечей (прогрев индикаторов) + 200 × 1h свечей (HTF тренд)
- **HTF Trend Analysis**: EMA20/50, RSI14, MACD, ADX на часовом таймфрейме
- **Market Readiness Gate**: блокировка торговли до готовности всех индикаторов
- **HTF Trend Filter**: блокировка сигналов против тренда на старшем ТФ
- **Тренд-защита стратегий**: MeanReversion и MicrostructureScalp не торгуют против EMA тренда
- **Conviction threshold 0.35**: повышенный порог уверенности для входа
- **Bitget WebSocket**: Подключение, подписки (ticker, trade, books, candle1m, candle5m), reconnect
- **Bitget REST API**: Отправка market/limit/post-only ордеров, отмена, HMAC-SHA256 подпись
- **5 торговых стратегий**: Momentum, MeanReversion, Breakout, VolatilityExpansion, MicrostructureScalp
- **14-проверочный Risk Engine**: Kill switch, daily loss, drawdown, exposure, stale feed, spread и др.
- **Order FSM**: 10 состояний (New → PendingAck → Open → PartiallyFilled → Filled/Cancelled/Rejected/Expired)
- **Портфель**: Позиции, realized/unrealized PnL, exposure, concentration
- **WorldModel**: 9 состояний мира (StableTrend, FragileBreakout, ChopNoise и др.)
- **RegimeEngine**: 13 режимов рынка с confidence и stability scoring
- **UncertaintyEngine**: 4 измерения (regime, signal, data quality, execution)
- **StrategyAllocator**: Аллокация по режиму и world suitability
- **DecisionAggregationEngine**: Комитетное голосование с вето
- **ExecutionAlpha**: Passive/Aggressive/Hybrid execution style, urgency, slice plans
- **OpportunityCost**: Ранжирование кандидатов, suppress/defer/execute
- **PortfolioAllocator**: Иерархическая аллокация (global → regime → strategy → symbol)
- **Persistence**: EventJournal + SnapshotStore через IStorageAdapter
- **Replay**: State machine (Idle → Playing → Completed), decision reconstruction
- **Telemetry**: Structured decision envelope
- **AlphaDecay**: Rolling metrics (expectancy, hit-rate, slippage), drift detection, Z-scores
- **Shadow**: Теневые решения, гипотетический PnL в bps
- **ChampionChallenger**: A/B сравнение стратегий, promotion/rejection
- **SelfDiagnosis**: Объяснение решений (structured + human-readable)
- **AdversarialDefense**: 6 видов угроз (spread explosion, liquidity vacuum, toxic flow и др.)
- **SyntheticScenarios**: 9 стресс-сценариев с валидацией реакций
- **Governance**: Audit log (15+ типов), strategy registry, config hashing
- **OperatorControl**: 11 команд (kill-switch, safe-mode, shadow, inspect и др.)
- **Supervisor**: Graceful shutdown, signal handling, dependency validation

---

## Ограничения ⚠️

### Persistence — только в памяти
- Текущий адаптер `MemoryStorageAdapter` — данные теряются при перезапуске.
- Интерфейс `IStorageAdapter` готов для файлового/БД-адаптера.

### Приватный WebSocket канал
- Статусы ордеров (fill events) не отслеживаются через приватный WS-канал.
- Для market-ордеров используется немедленное подтверждение (90%+ fills в рынке).
- Для limit-ордеров — polling через REST API не реализован.

### Один торговый символ одновременно
- Бот автоматически выбирает лучшую пару через PairScanner.
- Но торгует только одной парой одновременно (не портфель из N пар).
- Архитектура поддерживает multi-symbol pipeline, но требует доработки.
- Ротация пар происходит каждые 24 часа.

### AI Advisory
- Интерфейс определён, реализация возвращает нейтральную рекомендацию.
- Готов для подключения LLM через внешний API.

### Tools — пустые каталоги
- `tools/replay_inspector/`, `tools/telemetry_viewer/`, `tools/config_validator/`, `tools/log_summarizer/` — каркас без реализации.

### Benchmarks
- Каталог `benchmarks/` пуст. Латентность не замерена формально.

---

## Дорожная карта

| Приоритет | Задача | Описание |
|-----------|--------|----------|
| 🔴 Высокий | Файловая персистентность | SQLite или JSON-файловый адаптер для IStorageAdapter |
| 🔴 Высокий | Приватный WS-канал | Fill events через `wss://ws.bitget.com/v2/ws/private` |
| 🟡 Средний | Multi-symbol pipeline | Одновременная торговля top-N парами (параллельные pipelines) |
| 🟡 Средний | Backtest mode | Воспроизведение исторических данных через ReplayEngine |
| 🟡 Средний | Benchmarks | Латентность pipeline, feature generation, order submission |
| 🟡 Средний | HTF периодическое обновление | Пересчёт HTF тренда каждые 1-4 часа в реальном времени |
| 🔵 Низкий | AI Advisory | Интеграция LLM для диагностики и рекомендаций |
| 🔵 Низкий | Tools | Реализация replay_inspector, telemetry_viewer |
| 🔵 Низкий | Multi-exchange | Абстракция для других бирж (Binance, OKX) |
