# Поток принятия торговых решений

## Обзор

Этот документ описывает «интеллектуальный слой» системы Tomorrow Bot — от анализа рыночного состояния
до формирования торговых намерений, многоуровневой фильтрации и итогового решения.

## Архитектура конвейера

```
PairScanner → выбор лучшей пары → TradingPipeline(symbol)
                                        │
Bootstrap: 200×1m + 200×1h свечей       │
                                        ▼
                              Market Readiness Gate
                              (HTF valid + warmup 200 ticks + RSI norm)
                                        │
                                        ▼
FeatureSnapshot
    ├─→ WorldModelEngine      → WorldModelSnapshot
    ├─→ RegimeEngine          → RegimeSnapshot
    │
    └─→ UncertaintyEngine v2  → UncertaintySnapshot
         (принимает FeatureSnapshot + RegimeSnapshot + WorldModelSnapshot
          + PortfolioSnapshot + MlSignalSnapshot)
         9 измерений, stateful: EMA, hysteresis, shock memory, cooldown
         → execution mode recommendations, top drivers, diagnostics

AdvancedFeatureEngine
    ├─→ CUSUM                 → regime change signal
    ├─→ VPIN                  → toxicity score
    ├─→ Volume Profile        → POC + Value Area
    └─→ Time-of-Day Alpha    → volatility mult + alpha score

Entropy Filter (quality gate) ← блокирует при entropy > 0.85
    │
    ▼
Стратегии (с тренд-фильтрацией)
    IStrategy::evaluate(StrategyContext) → [TradeIntent...]
    ├── MeanReversion: не BUY если EMA20<EMA50 + ADX>20, требует MACD reversal
    ├── MicrostructureScalp: не BUY если EMA20<EMA50 (медвежий тренд)
    └── Momentum/Breakout/VolExpansion: встроенные фильтры

Fingerprint Check ← блокирует при edge < -0.1 (3125 паттернов)
    │
    ▼
StrategyAllocator
    allocate(strategies, regime, world, uncertainty) → AllocationResult
    (определяет веса и активность каждой стратегии)
    + Bayesian Online Adaptation (обновление параметров в реальном времени)

DecisionAggregationEngine
    aggregate(symbol, intents, allocation, regime, world, uncertainty,
             ai_advisory?, portfolio?, features?) → DecisionRecord
    (комитетное голосование с regime-adaptive thresholds, ensemble conviction,
     time decay, execution cost modeling, portfolio awareness)

Cascade Check (аварийная блокировка) ← score > 0.6 → блок всех входов
    │
    ▼
Correlation Risk Adjustment ← decorrelation → risk_mult 0.5
    │
    ▼
Thompson Sampling Entry Timing ← 5-arm bandit (EnterNow, Wait1/2/3, Skip)
    │
    ▼
HTF Trend Filter (финальный барьер)
    BUY блокирован: htf_trend=-1 + strength>0.4 без разворота (MACD>0 + RSI>40)
    SELL блокирован: htf_trend=+1 + strength>0.4 без разворота (MACD<0 + RSI<60)

→ Volatility Targeting (сайзинг) → Chandelier Exit (trailing stop)
→ RiskEngine (14 проверок) → TWAP Executor → ExecutionEngine
→ Alpha Decay Feedback (каждые 60с) → обратная связь в StrategyAllocator
```

## Модули

### 1. WorldModelEngine (`tb::world_model`)

Классифицирует глобальное состояние рынка в одно из 9 состояний:

| Состояние | Описание |
|-----------|----------|
| StableTrendContinuation | Устойчивый тренд — безопасно для momentum |
| FragileBreakout | Хрупкий пробой — может откатиться |
| CompressionBeforeExpansion | Сжатие → ожидание расширения |
| ChopNoise | Шум без направления |
| ExhaustionSpike | Спайк истощения — возможен разворот |
| LiquidityVacuum | Вакуум ликвидности — торговля опасна |
| ToxicMicrostructure | Токсичная микроструктура |
| PostShockStabilization | Стабилизация после шока |
| Unknown | Недостаточно данных |

Каждое состояние маппится на упрощённый `WorldStateLabel` для обратной совместимости.

### 2. RegimeEngine (`tb::regime`)

Классифицирует рыночный режим в один из 13 детализированных режимов.
Генерирует рекомендации (`RegimeStrategyHint`) для каждой стратегии:
- `should_enable` — включить/отключить стратегию
- `weight_multiplier` — множитель веса

### 3. UncertaintyEngine v2 (`tb::uncertainty`)

9-мерная stateful оценка неопределённости с нелинейной агрегацией.

**Измерения:**

| # | Измерение | Вес | Источник |
|---|-----------|-----|----------|
| 1 | Regime | 0.25 | RegimeSnapshot (1 - confidence) |
| 2 | Signal | 0.20 | FeatureSnapshot (конфликты RSI/MACD/EMA) |
| 3 | Data Quality | 0.10 | FeatureSnapshot (стакан, свежесть, спред) |
| 4 | Execution | 0.10 | FeatureSnapshot (спред, ликвидность, slippage) |
| 5 | Portfolio | 0.10 | PortfolioSnapshot (концентрация, exposure, drawdown) |
| 6 | ML | 0.10 | MlSignalSnapshot (signal quality, cascade) |
| 7 | Correlation | 0.05 | MlSignalSnapshot (correlation breaks) |
| 8 | Transition | 0.05 | RegimeSnapshot (transition confidence, stability) |
| 9 | Operational | 0.05 | FeatureSnapshot (feed freshness, infrastructure) |

**Stateful модель:**
- EMA-сглаживание (`persistent_score`) для фильтрации шума
- Гистерезис (up: 0.03, down: 0.05) — предотвращает осцилляцию между уровнями
- Shock memory — отслеживает серии экстремальных оценок
- Cooldown — рекомендация паузы после 3+ Extreme подряд

**Агрегация:** взвешенная сумма + regime-specific amplifiers (volatile: data/exec ×1.5; unclear: all ×1.2) + tail stress (+0.05 за каждый dimension > 0.8).

**Интеграции:**
- Risk Engine: 3 uncertainty-aware проверки (limits, cooldown, execution mode)
- Execution Alpha: модификация стиля и urgency
- Execution Engine: size/threshold guards

**Execution mode recommendations:** Normal → Conservative → DefensiveOnly → HaltNewEntries

Результат → `UncertaintyAction`: Normal / ReducedSize / HigherThreshold / NoTrade

[Подробная документация →](../../docs/architecture/uncertainty_engine.md)

### 4. Стратегии (`tb::strategy`)

5 встроенных стратегий с тренд-фильтрацией:

| Стратегия | Сигнал | Предпочтительный режим | Защита |
|-----------|--------|----------------------|--------|
| Momentum | EMA crossover + RSI + ADX | Trending | ADX > 25 (встроенная) |
| MeanReversion | BB + RSI экстремумы | Ranging | Требует MACD reversal, не BUY в даунтренде |
| Breakout | BB сжатие→расширение | Volatile | Направленный по тренду |
| MicrostructureScalp | Дисбаланс стакана | Ranging (узкий спред) | Не торгует против EMA тренда |
| VolExpansion | ATR расширение | Volatile | Нейтральный RSI (40-60) |

**Ключевой принцип**: стратегии НЕ размещают ордера — они генерируют `TradeIntent`.

**Защита Mean Reversion от «ловли падающего ножа»:**
1. Если EMA20 < EMA50 + ADX > 20 → сильный даунтренд → BUY подавлен
2. Исключение: MACD histogram > 0 (разворот) или RSI > 25 (начало восстановления)
3. RSI < 15 → паника → BUY заблокирован полностью
4. Conviction снижается на 30% при торговле против тренда
5. Conviction повышается на 20% при подтверждении MACD reversal

**Защита Microstructure Scalp:**
1. EMA20 < EMA50 → медвежий тренд → BUY скальп заблокирован
2. EMA20 > EMA50 → бычий тренд → SELL скальп заблокирован
3. Conviction +15% при торговле по тренду

### 5. StrategyAllocator (`tb::strategy_allocator`)

Алгоритм аллокации:
1. Начальный вес = 1.0
2. × множитель из RegimeStrategyHint
3. × suitability из WorldModelSnapshot
4. × size_multiplier из UncertaintySnapshot
5. Отключить если вес < 0.1
6. Нормализовать оставшиеся веса (сумма = 1.0)

### 6. DecisionAggregationEngine (`tb::decision`)

Профессиональный комитетный движок принятия решений с адаптивными порогами,
ансамблевой conviction, учётом стоимости исполнения и портфельным контекстом.

**Сигнатура:**
```cpp
aggregate(symbol, intents, allocation, regime, world, uncertainty,
          ai_advisory?, portfolio?, features?) → DecisionRecord
```

**Конвейер решения (10 этапов):**

1. **Time-skew detection** — проверяет рассинхронизацию между regime/uncertainty
   снапшотами (порог 200мс). Предупреждение в лог при обнаружении.

2. **Глобальные вето:**
   - `UncertaintyAction::NoTrade` → полный запрет
   - Все стратегии отключены аллокатором
   - AI Advisory с severity ≥ порог вето

3. **Execution cost estimation** — моделирует стоимость входа:
   - `spread_bps` из микроструктуры
   - `slippage_bps` из execution context
   - Вето если `total_cost_bps > max_acceptable_cost_bps` (по умолчанию 80 bps)

4. **Scoring с time decay + cost penalty:**
   - Time decay: `conviction × exp(-ln(2)/halflife × age_ms)` (halflife 700мс)
   - Execution cost penalty: `(spread+slippage)/10000 × 0.5`
   - `weighted_score = effective_conviction × weight`

5. **Разрешение конфликтов BUY/SELL:**
   - Regime-adaptive dominance threshold (55% в тренде, 80% в стрессе)
   - Вето при отсутствии доминирующего направления

6. **Эффективный порог conviction:**
   - Base threshold × uncertainty.threshold_multiplier
   - × regime factor (0.90 для StrongUptrend … 1.50 для AnomalyEvent)
   - + drawdown boost (до +0.25 при глубокой просадке + серии убытков)

7. **AI Advisory adjustment** — коррекция conviction по AI рекомендациям

8. **Ensemble conviction bonus:**
   - Бонус за согласие нескольких стратегий (diminishing returns: 0.08 × 0.6^i)
   - Максимум +0.20 от ансамбля
   - Записывается в `EnsembleMetrics` (aligned_count, agreement_ratio, bonus)

9. **Threshold check** — `adjusted_conviction ≥ effective_threshold`
   - `approval_gap = conviction - threshold` (для near-miss анализа)
   - Structured `RejectionReason` при отказе

10. **Формирование DecisionRecord** с полным rationale для аудита

**Таблица режимных множителей conviction threshold:**

| Режим | Множитель | Логика |
|-------|-----------|--------|
| StrongUptrend/Downtrend | 0.90 | В сильном тренде порог снижен |
| WeakUptrend/Downtrend | 1.00 | Нейтральный |
| MeanReversion | 1.10 | Осторожность при возврате к среднему |
| VolatilityExpansion | 0.95 | Лёгкое снижение (движения быстрые) |
| LowVolCompression | 1.05 | Слегка повышен (мало возможностей) |
| LiquidityStress | 1.25 | Значительно повышен |
| SpreadInstability | 1.20 | Нестабильный спред — осторожность |
| Chop | 1.35 | Шум — высокий порог |
| AnomalyEvent | 1.50 | Аномалия — максимальная осторожность |
| ToxicFlow | 1.40 | Токсичный поток — почти запрет |

**Категории отклонения (RejectionReason):**
- `None` — одобрено
- `LowConviction` — conviction ниже порога
- `GlobalUncertaintyVeto` — экстремальная неопределённость
- `SignalConflict` — конфликт BUY/SELL без доминирования
- `AllStrategiesDisabled` — аллокатор отключил все стратегии
- `NoValidIntents` — нет торговых намерений
- `ExecutionCostTooHigh` — издержки исполнения выше допустимого
- `AIAdvisoryVeto` — AI рекомендует воздержаться
- `DrawdownLimit` — просадка превышает лимит
- `RegimeUnsafe` — режим не подходит для торговли

**Все фичи управляются через `AdvancedDecisionConfig`** (все включены по умолчанию).

### 7. HTF Trend Filter (новый, финальный барьер)

После одобрения решения DecisionEngine, перед отправкой ордера применяется
финальный фильтр на основе анализа старшего таймфрейма (1h):

| Условие | Действие |
|---------|----------|
| BUY + HTF trend = -1 + сила > 0.4 | Заблокирован (кроме: MACD > 0 И RSI > 40) |
| SELL + HTF trend = +1 + сила > 0.4 | Заблокирован (кроме: MACD < 0 И RSI < 60) |
| HTF невалиден | Пропуск фильтра (не блокирует) |

### 8. Market Readiness Gate (новый)

Шлюз готовности рынка — блокирует ВСЮ торговлю до выполнения условий:

1. **HTF индикаторы валидны** — 200 часовых свечей загружены и обработаны
2. **Прогрев завершён** — минимум 200 тиков (≈3-5 минут live данных)
3. **RSI не в экстремальной зоне** — HTF RSI между 15 и 85
4. **Нет сильного даунтренда** — если HTF trend = -1 + сила > 0.6, требуется MACD > 0 или RSI > 35

### 9. AIAdvisoryEngine (`tb::ai`)

Заглушка для будущего подключения ML/LLM сервиса. Текущая реализация всегда возвращает `nullopt`.

### 10. Entropy Filter (`tb::ml`, новый — quality gate)

Shannon entropy вычисляется на 4 каналах: returns, volume, spread, order flow.
При entropy > 0.85 (высокий шум, рынок непредсказуем) — торговля блокируется.
Результат кешируется для эффективности. Вызывается **перед** оценкой стратегий.

### 11. Microstructure Fingerprinting (`tb::ml`, новый)

Формирует 5D fingerprint текущей микроструктуры:
- Spread (5 buckets), Imbalance (5), Flow (5), Volatility (5), Depth (5)
- Итого: 5^5 = 3125 уникальных паттернов
- Knowledge base с win_rate для каждого паттерна
- Блокирует вход при edge < -0.1
- Вызывается **после** генерации стратегий, **перед** conviction threshold

### 12. Liquidation Cascade Detector (`tb::ml`, новый — аварийная блокировка)

Детектирует каскадные ликвидации по трём сигналам:
- Velocity (вес 40%) — скорость движения цены
- Volume spike (вес 30%) — аномальный объём
- Depth thinning (вес 30%) — истончение стакана

При composite score > 0.6 — **аварийная блокировка** всех входов.

### 13. Correlation Monitor (`tb::ml`, новый)

Вычисляет Pearson correlation с BTC и ETH в двух окнах:
- Short window (20 тиков) — быстрая реакция
- Long window (100 тиков) — базовая корреляция

При decorrelation (резкое расхождение short vs long) → risk_mult = 0.5.
При отсутствии данных — нейтральная оценка (не блокирует).

### 14. Bayesian Online Adaptation (`tb::ml`, новый)

Normal-Normal conjugate prior для онлайн обновления параметров стратегий:
- Regime-conditioned: 70% вес текущего режима, 30% общие данные
- Thompson Sampling exploration (10%) — исследование альтернативных параметров
- Подаёт обновлённые параметры в StrategyAllocator

### 15. Thompson Sampling Entry Timing (`tb::ml`, новый)

5-arm Beta bandit для оптимизации момента входа:
- Arms: EnterNow, Wait1, Wait2, Wait3, Skip
- Каждый arm имеет Beta(α, β) distribution
- Exponential decay 0.995 для адаптации к non-stationarity
- Reward = realized PnL от решения

## Потокобезопасность

Все движки хранят состояние в `std::unordered_map` с защитой `std::mutex`.
Публичные методы используют `std::lock_guard`.

## Replay-совместимость

Все структуры содержат `Timestamp` и `CorrelationId` для воспроизведения решений.
`DecisionRecord::is_reconstructable()` проверяет наличие всех необходимых данных.
