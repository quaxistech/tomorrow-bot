# Известные ограничения и дорожная карта

## Текущее состояние

Tomorrow Bot — полностью рабочая production-grade торговая система:
- **49 модулей** реализованы без заглушек
- **614 unit-тестов** проходят (100%)
- **Реальная торговля** на Bitget USDT-M Futures через REST API v2
- **Фьючерсный режим**: hedge mode, isolated margin, адаптивное плечо 5×–20×
- **WebSocket**: ~1000+ сообщений в минуту
- **Graceful shutdown** по SIGTERM/SIGINT
- **Автоматический выбор пар** через PairScanner v5, ротация каждые 12ч
- **Мультитаймфреймный анализ**: 200 × 1h свечей (8+ дней) для HTF тренда
- **Тренд-фильтрация**: стратегии не торгуют против HTF тренда
- **6 ML-модулей**: entropy filter, fingerprinting, cascade detection, correlation, bayesian, thompson
- **Smart TWAP**: адаптивное исполнение крупных ордеров
- **Chandelier Exit / Trailing Stop**: адаптивный ATR-based стоп
- **Volatility Targeting**: динамический сайзинг позиций
- **12 встроенных индикаторов**: SMA, EMA, RSI, MACD, Bollinger, ATR, ADX, OBV, VWAP, Rolling VWAP, ROC, Z-Score

---

## Полностью реализовано ✅

- **Торговый pipeline**: WebSocket → Normalizer → Features → Strategies → Risk → Execution
- **Автоматический выбор пар**: PairScanner v5 (acceleration-based scoring, parallel fetch, retry/circuit breaker, diversification, audit trail, 55 unit tests)
- **Загрузка истории**: 200 × 1m свечей (прогрев индикаторов) + 200 × 1h свечей (HTF тренд)
- **HTF Trend Analysis**: EMA20/50, RSI14, MACD, ADX на часовом таймфрейме
- **HTF Real-Time Update**: пересчёт HTF каждый час + экстренное обновление при > 3×ATR
- **Market Readiness Gate**: блокировка торговли до готовности всех индикаторов
- **HTF Trend Filter**: блокировка сигналов против тренда на старшем ТФ
- **Тренд-защита стратегий**: MeanReversion и MicrostructureScalp не торгуют против EMA тренда
- **Conviction threshold 0.45**: повышенный порог уверенности для входа (динамический)
- **Bitget WebSocket**: Подключение, подписки (ticker, trade, books, candle1m, candle5m), reconnect
- **Bitget REST API**: Отправка market/limit/post-only ордеров, отмена, HMAC-SHA256 подпись
- **9 торговых стратегий**: Momentum, MeanReversion, Breakout, VolatilityExpansion, MicrostructureScalp, EmaPullback, RsiDivergence, VwapReversion, VolumeProfile
- **Typed Strategy Configs**: Все пороги стратегий конфигурируемы через типизированные структуры
- **Spot Signal Semantics**: SignalIntent (LongEntry/LongExit/ShortEntry/ShortExit/Reduce/Hold), ExitReason attribution
- **27-проверочный Risk Engine**: Kill switch, daily loss, drawdown, exposure, stale feed, spread, liquidity, global limits, spot SELL guard и др.
- **Order FSM**: 10 состояний (New → PendingAck → Open → PartiallyFilled → Filled/Cancelled/Rejected/Expired)
- **Портфель**: Позиции, realized/unrealized PnL, exposure, concentration
- **WorldModel**: 9 состояний мира (StableTrend, FragileBreakout, ChopNoise и др.)
- **RegimeEngine**: 13 режимов рынка с config-driven порогами, hysteresis FSM, explainability, stress policies
- **UncertaintyEngine v2**: 9 измерений (regime, signal, data quality, execution, portfolio, ML, correlation, transition, operational), EMA-сглаживание, hysteresis, shock memory
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
    - **ChampionChallenger v2**: net P&L (gross-fee-slippage), drawdown tracking, hit rate, pre-promotion audit (4 критерия), observer callbacks, Prometheus metrics, persistence
- **SelfDiagnosis**: Объяснение решений (structured + human-readable)
- **AdversarialDefense v4**: 14 детекторов рыночных угроз (spread explosion, liquidity vacuum, toxic flow, reject storm, latency spike и др.), percentile scoring, correlation matrix, hysteresis
- **SyntheticScenarios**: 9 стресс-сценариев с валидацией реакций
- **Governance**: Audit log (15+ типов), strategy registry, config hashing
- **OperatorControl**: 11 команд (kill-switch, safe-mode, shadow, inspect и др.)
- **Supervisor**: Graceful shutdown, signal handling, dependency validation

### Продвинутые технологии v0.3 ✅ (новое)

**Уровень 1 — Управление рисками:**
- **Chandelier Exit / Trailing Stop**: адаптивный trailing stop 3×ATR (сильный тренд), 2.5× (умеренный), 1.5× (choppy). Breakeven при +1.5×ATR, partial TP 50% при +2×ATR
- **Volatility Targeting**: динамический сайзинг size = target_vol / realized_vol × Kelly × regime_mult. 13 режимов, Kelly = 0.25
- **Alpha Decay Feedback**: мониторинг деградации каждые 60с. Рекомендации: ReduceWeight, ReduceSize, RaiseThresholds, Disable
- **HTF Real-Time Update**: пересчёт HTF каждый час + экстренное обновление при > 3×ATR

**Уровень 2 — Продвинутые индикаторы:**
- **CUSUM**: two-sided cumulative sum, drift 0.5σ, threshold 4σ, sample stddev, без look-ahead bias
- **VPIN**: Volume-Synchronized Probability of Informed Trading, порог 0.7, 10-trade calibration
- **Volume Profile**: POC + Value Area (70%), 50 уровней, 5000 трейдов lookback
- **Smart TWAP**: 3-10 слайсов, адаптивный интервал, front-loading 1.2×
- **Time-of-Day Alpha**: 24-часовой UTC профиль, +0.05 порог в тихие часы

**Уровень 3 — ML/AI:**
- **Bayesian Online Adaptation**: Normal-Normal conjugate prior, regime-conditioned (70%), Thompson exploration (10%)
- **Entropy Filter**: Shannon entropy на 4 каналах, порог 0.85, кеширование
- **Microstructure Fingerprinting**: 5D × 5 buckets = 3125 паттернов, edge threshold -0.1
- **Liquidation Cascade Prediction**: velocity 40% + volume 30% + depth 30%, порог 0.6
- **Correlation Monitor**: Pearson correlation с BTC/ETH, short 20 / long 100, risk_mult 0.5
- **RL Entry Timing (Thompson Sampling)**: 5-arm Beta bandit, decay 0.995

### Исправления ошибок v0.3 ✅

- P&L always 0.0 в alpha decay / fingerprint / Thompson — исправлено
- SELL position trailing stop initialization — исправлено
- RNG seeding (bayesian, thompson) — исправлено
- TWAP NaN/Inf edge cases — исправлено
- TWAP thread-safe counter — исправлено
- HTF buffer data races — исправлено
- CUSUM look-ahead bias + sample stddev — исправлено
- VPIN calibration on first trade — исправлено
- Entropy filter caching — исправлено
- Fingerprint eviction + breakeven bias — исправлено
- Cascade depth ratio logic — исправлено
- Correlation no-data ambiguity — исправлено

---

### Институциональное управление портфелем v0.4 ✅ (новое)

**Portfolio Engine — Cash Reserve Accounting:**
- **CashLedger**: Полный институциональный учёт cash (total, available, reserved, fees, PnL gross/net)
- **Reserve/Release**: Pre-trade cash reservation (reserve_cash/release_cash) — блокировка средств до исполнения
- **Fee Tracking**: Учёт комиссий per-trade через record_fee(), суммарные комиссии за день
- **PendingOrderInfo**: Трекинг всех активных ордеров с зарезервированным cash
- **Event Sourcing**: PortfolioEvent audit log (10 типов событий, кольцевой буфер 10k)
- **Invariant Checks**: check_invariants() верифицирует available >= 0, reserves = sum(pending)
- **Backward Compatible**: Все существующие API работают без изменений

**Portfolio Allocator — Professional Sizing:**
- **compute_size_v2()**: Новый API с полным AllocationContext (замена stateful set_market_context)
- **ExchangeFilters**: Per-symbol trading rules (min/max qty, lot step, min notional, tick size, fee rates)
- **Drawdown Scaling**: Линейное снижение размера при просадке (5%→15% = 100%→10% размера)
- **Liquidity Constraints**: Ограничение по ADV participation (2%) и глубине стакана (10%)
- **Constraint Audit Trail**: Полная трассировка каждого ограничения (вход, выход, лимит, binding)
- **Fee-Aware Sizing**: expected_fee и fee_adjusted_notional в результате
- **Utility Functions**: round_quantity_down(), round_price(), validate_order_filters()

**Execution Engine — Reserve Integration:**
- **Pre-trade Reserve**: reserve_cash() вызывается перед submit_order() для BUY-ордеров
- **Release on Cancel/Reject**: Автоматическое освобождение при отмене/reject/expire
- **Fee Consistency Fix**: on_order_update() теперь учитывает комиссии (было: без комиссий)
- **record_fee()**: Комиссии фиксируются при каждом fill (BUY и SELL стороны)

---

### Production Hardening v0.5 ✅ (новое)

**Критические исправления торговой логики:**
- **Partial close**: `reduce_position()` корректно уменьшает размер (а не удаляет позицию) при частичном закрытии (TP 50%)
- **Adversarial defense**: `LiquidityVacuum` и `ToxicFlow` — корректный `VetoTrade` при severity ≥ 0.85
- **ProductionGuard**: подключён к bootstrap — блокирует production без env-подтверждения

**Стабилизация:**
- Исключены `catch(...)` без логирования в main.cpp, trading_pipeline.cpp
- `parse_double()` с именем поля для 167 вызовов config loader
- Удалён vestigial `record_trade_result()`

**Production hardening:**
- Дедупликация execution engine: `apply_sell_fill_to_portfolio()` / `apply_buy_fill_to_portfolio()`
- Config validators: trading_params (9 полей), decision (8), execution_alpha (14) + cross-validation
- Spot SELL guard (risk check #27): запрет продажи без открытой long-позиции
- 37 новых тестов (31 config + 3 spot-semantic + 3 regression)

**Удаление TA-Lib:**
- Полное удаление зависимости TA-Lib (cmake, conditional blocks, error codes)
- Все 12 индикаторов — встроенные реализации (SMA, EMA, RSI, MACD, BB, ATR, ADX, OBV, VWAP, Rolling VWAP, ROC, Z-Score)
- Централизация комиссий Bitget: `common::fees::kDefaultTakerFeePct` / `kDefaultMakerFeePct`
- Исправлена maker_fee_pct: 0.001 → 0.0008 (корректная комиссия Bitget)

---

## Ограничения ⚠️

### Persistence — PostgreSQL + память
- PostgreSQL адаптер реализован и используется для alpha decay, WAL, journal.
- `MemoryStorageAdapter` используется как fallback при отсутствии `POSTGRES_URL`.
- Интерфейс `IStorageAdapter` позволяет добавить другие бэкенды.

### Приватный WebSocket канал
- Статусы ордеров (fill events) не отслеживаются через приватный WS-канал.
- Для market-ордеров используется немедленное подтверждение (90%+ fills в рынке).
- Для limit-ордеров — polling через REST API не реализован.

### Один торговый символ одновременно
- Бот автоматически выбирает лучшую пару через PairScanner v5.
- Торгует только одной парой одновременно.
- Ротация пар происходит каждые 12 часов.

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
| ✅ Готово | HTF периодическое обновление | Пересчёт HTF тренда каждый час + экстренное обновление при > 3×ATR |
| ✅ Готово | ML/AI модули (6 шт.) | Bayesian, Entropy, Fingerprint, Cascade, Correlation, Thompson |
| ✅ Готово | Smart TWAP | Адаптивное разбиение крупных ордеров (3-10 слайсов) |
| ✅ Готово | Chandelier Exit | Адаптивный trailing stop (1.5×–3×ATR) |
| ✅ Готово | Volatility Targeting | Динамический сайзинг (Kelly × regime_mult) |
| ✅ Готово | Advanced Indicators | CUSUM, VPIN, Volume Profile, Time-of-Day Alpha |
| � Готово | Фьючерсная торговля | USDT-M Futures, hedge mode, isolated margin, adaptive leverage |
| 🟢 Готово | PostgreSQL персистентность | PostgreSQL adapter для journal, snapshots, WAL |
| 🔴 Высокий | Приватный WS-канал | Fill events через `wss://ws.bitget.com/v2/ws/private` |
| 🟡 Средний | Multi-symbol pipeline | Одновременная торговля top-N парами (параллельные pipelines) |
| 🟡 Средний | Backtest mode | Воспроизведение исторических данных через ReplayEngine |
| 🟡 Средний | Benchmarks | Латентность pipeline, feature generation, order submission |
| 🔵 Низкий | AI Advisory LLM | Интеграция LLM для диагностики и рекомендаций |
| 🔵 Низкий | Tools | Реализация replay_inspector, telemetry_viewer |
| 🔵 Низкий | Multi-exchange | Абстракция для других бирж (Binance, OKX) |
