# Торговый конвейер (TradingPipeline)

## Обзор

`TradingPipeline` — центральный компонент системы. Один pipeline обслуживает один символ.
Создаётся в `main.cpp` для каждого выбранного ScannerEngine символа.

```cpp
TradingPipeline(config, secret_provider, logger, clock, metrics, symbol);
```

Pipeline является tick-driven: каждый тик (новый FeatureSnapshot из WebSocket) запускает
полный цикл обработки.

---

## Компоненты pipeline

Pipeline создаёт и владеет всеми внутренними компонентами:

| Компонент | Тип |
|-----------|-----|
| Market data | `MarketDataGateway`, `Normalizer`, `LocalOrderBook` |
| Features | `IndicatorEngine`, `FeatureEngine`, `AdvancedFeatureEngine` |
| Intelligence | `WorldModelEngine`, `RegimeEngine`, `UncertaintyEngine` |
| Strategy | `StrategyRegistry`, `StrategyAllocator`, `StrategyEngine` |
| Decision | `CommitteeDecisionEngine` |
| ML | `BayesianAdapter`, `EntropyFilter`, `MicrostructureFingerprinter`, `LiquidationCascadeDetector`, `CorrelationMonitor`, `ThompsonSampler` |
| Execution prep | `ExecutionAlphaEngine`, `OpportunityCostEngine`, `PortfolioAllocator` |
| Leverage | `LeverageEngine` |
| Risk | `ProductionRiskEngine` |
| Execution | `ExecutionEngine`, `SmartTwapExecutor` |
| Portfolio | `PortfolioEngine` |
| Recovery | `ReconciliationEngine`, `OrderWatchdog` |

---

## Этапы обработки тика

### 1. Получение данных

WebSocket → `MarketDataGateway` → callback `on_feature_snapshot()`

Данные: ticker, trade, orderbook update, candle (1m, 5m).

### 2. Нормализация

`Normalizer` преобразует сырые данные Bitget во внутренние структуры.
`LocalOrderBook` обновляет L2-стакан.

### 3. Feature extraction

`IndicatorEngine` обновляет 12+ индикаторов (RSI, EMA, MACD, ATR, ADX, BB, OBV, VWAP...).

`FeatureEngine` собирает `FeatureSnapshot` — полную картину рынка на текущий момент.

`AdvancedFeatureEngine` добавляет CUSUM, VPIN, Volume Profile, Time-of-Day alpha.

### 4. Market intelligence

| Компонент | Вход | Выход |
|-----------|------|-------|
| `WorldModelEngine` | FeatureSnapshot | `WorldModelSnapshot` — 9 состояний, fragility |
| `RegimeEngine` | FeatureSnapshot | `RegimeSnapshot` — режим, confidence, stability |
| `UncertaintyEngine` | FeatureSnapshot, RegimeSnapshot | `UncertaintySnapshot` — 9 измерений |

### 5. Strategy allocation

`StrategyAllocator` назначает веса стратегиям на основе текущего режима и world model.

### 6. Strategy evaluation

`StrategyEngine` оценивает текущую ситуацию через state machine.
Возвращает `optional<TradeIntent>` — торговое намерение или ничего.

4 сценария: momentum continuation, retest, pullback, rejection.

### 7. ML enrichment

```
FeatureSnapshot + TradeIntents
        │
        ▼
┌─ EntropyFilter ──── шумит? → block
├─ LiquidationCascade ── каскад? → block
├─ CorrelationMonitor ── decorrelation? → risk_mult
├─ Fingerprinter ──── edge < -0.1? → block
├─ BayesianAdapter ── параметры стратегии
└─ ThompsonSampler ── оптимальный момент входа
        │
        ▼
  MlSignalSnapshot (агрегированный результат)
```

### 8. Decision aggregation

`CommitteeDecisionEngine::aggregate()` — восемь параметров:

1. `symbol` — торговый символ
2. `intents` — список торговых намерений от стратегий
3. `allocation` — веса стратегий
4. `regime` — текущий режим
5. `world` — состояние мира
6. `uncertainty` — неопределённость
7. `portfolio` (опционально)
8. `features` (опционально)

Результат: `DecisionRecord` — approved/rejected, conviction, причина.

### 9. Execution preparation

| Компонент | Функция |
|-----------|---------|
| `ExecutionAlphaEngine` | Выбор execution style (passive/aggressive/hybrid) |
| `OpportunityCostEngine` | Ранжирование кандидатов по expected value |
| `PortfolioAllocator` | Определение размера позиции |

### 10. Leverage

`LeverageEngine` вычисляет оптимальное плечо (1×–20×) на основе 7 факторов.
Синхронизирует leverage с биржей если изменилось.

### 11. Risk check

`ProductionRiskEngine::evaluate()` — 33 sequential policy checks.
Любая проверка может отклонить ордер.

### 12. Execution

`ExecutionEngine::execute()` отправляет ордер на биржу через `IOrderSubmitter`.
Крупные ордера разбиваются `SmartTwapExecutor` на 3–10 слайсов.

### 13. Post-trade

- `FillProcessor` обновляет `PortfolioEngine`
- `ReconciliationEngine` сверяет состояние с биржей
- `OrderWatchdog` отменяет зависшие ордера

---

## Жизненный цикл

```
          start()
             │
             ▼
   Подключение к WebSocket
             │
             ▼
   Подписка на каналы (ticker, trade, book, candle)
             │
             ▼
   Ожидание тиков ──── on_feature_snapshot() ──── полный цикл
             │
             ▼
          stop()
             │
             ▼
   Отмена открытых ордеров
   Отписка от каналов
   Закрытие WebSocket
```

---

## Multi-pipeline координация

- Supervisor регистрирует каждый pipeline как подсистему
- Symbol Lock Registry предотвращает конфликты между pipeline
- Kill switch broadcast останавливает все pipeline одновременно
- Global position limits ограничивают суммарное число позиций

---

## Hot-swap (ротация пар)

Idle Monitor (фоновый поток, каждые 5 мин):

1. Проверяет: все pipeline idle > 30 мин и нет открытых позиций?
2. Если да — запускает rescan через ScannerEngine
3. Останавливает старые pipeline
4. Создаёт новые pipeline для новых символов
5. Throttle: не чаще 1 rescan per 30 мин

При рестарте: проверяются held assets на бирже — символы с открытыми позициями добавляются принудительно.
