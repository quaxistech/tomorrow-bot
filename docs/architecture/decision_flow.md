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
    └─→ UncertaintyEngine     → UncertaintySnapshot
         (принимает FeatureSnapshot + RegimeSnapshot + WorldModelSnapshot)

Стратегии (с тренд-фильтрацией)
    IStrategy::evaluate(StrategyContext) → [TradeIntent...]
    ├── MeanReversion: не BUY если EMA20<EMA50 + ADX>20, требует MACD reversal
    ├── MicrostructureScalp: не BUY если EMA20<EMA50 (медвежий тренд)
    └── Momentum/Breakout/VolExpansion: встроенные фильтры

StrategyAllocator
    allocate(strategies, regime, world, uncertainty) → AllocationResult
    (определяет веса и активность каждой стратегии)

DecisionAggregationEngine
    aggregate(symbol, intents, allocation, regime, world, uncertainty) → DecisionRecord
    (голосование комитета с вето-логикой, conviction threshold = 0.35)

HTF Trend Filter (финальный барьер)
    BUY блокирован: htf_trend=-1 + strength>0.4 без разворота (MACD>0 + RSI>40)
    SELL блокирован: htf_trend=+1 + strength>0.4 без разворота (MACD<0 + RSI<60)

→ RiskEngine (14 проверок) → ExecutionEngine
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

### 3. UncertaintyEngine (`tb::uncertainty`)

Оценивает неопределённость по 5 измерениям:
- Режим (1 - confidence)
- Сигналы (конфликты индикаторов)
- Качество данных (стакан, свежесть)
- Исполнение (спред, проскальзывание)
- Портфель (Фаза 4)

Результат → `UncertaintyAction`: Normal / ReducedSize / HigherThreshold / NoTrade

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

Комитетное голосование:
1. Глобальные вето (UncertaintyAction::NoTrade, все стратегии отключены)
2. Для каждого интента: weighted_score = conviction × weight
3. Конфликт BUY/SELL → вето
4. Лучший weighted_score > threshold (0.35, с учётом uncertainty_threshold_multiplier)
5. Формирование `DecisionRecord` с полным объяснением

**Порог conviction повышен с 0.2 до 0.35** — требует более уверенных сигналов для входа.

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

## Потокобезопасность

Все движки хранят состояние в `std::unordered_map` с защитой `std::mutex`.
Публичные методы используют `std::lock_guard`.

## Replay-совместимость

Все структуры содержат `Timestamp` и `CorrelationId` для воспроизведения решений.
`DecisionRecord::is_reconstructable()` проверяет наличие всех необходимых данных.
