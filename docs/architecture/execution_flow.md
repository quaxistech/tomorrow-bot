# Поток исполнения ордеров — Фаза 4

## Обзор

Фаза 4 реализует полный путь от торгового решения до исполнения ордера.
Каждый шаг обязателен, ни один торговый путь не может обойти Риск-движок.

## Конвейер исполнения

```
DecisionRecord (из decision/)
    │
    ▼
ExecutionAlphaEngine → ExecutionAlphaResult
    │  Определяет стиль исполнения (Passive/Aggressive/Hybrid/PostOnly/NoExecution)
    │  Анализирует VPIN и дисбаланс стакана (book_imbalance_5)
    │  Рассчитывает срочность с учётом CUSUM и time-of-day
    │  Оценивает adverse selection (взвешенная агрегация: VPIN, flow, spread)
    │  Рассчитывает лимитную цену (weighted_mid_price + smart maker offset)
    │  Предоставляет plan нарезки (slice_plan) для TWAP executor
    │  Заполняет DecisionFactors — полная аудит-трасса каждого фактора
    │
    ▼
OpportunityCostEngine → OpportunityCostResult
    │  Оценивает ожидаемый доход vs стоимость исполнения
    │  Решает: Execute / Defer / Suppress
    │
    ▼
PortfolioAllocator → SizingResult
    │  Применяет иерархию бюджетов
    │  Лимит концентрации (≤20% капитала на одну позицию)
    │  Лимит стратегии (≤30% капитала)
    │  Множитель неопределённости
    │  Ограничение доступным капиталом
    │
    ▼
┌─────────────────────────────────────────┐
│     RiskEngine → RiskDecision           │
│     *** ОБЯЗАТЕЛЬНЫЙ, ОБХОД ЗАПРЕЩЁН ***│
│                                         │
│  14 правил последовательной проверки:   │
│  1.  Kill switch                        │
│  2.  Макс дневной убыток              │
│  3.  Макс просадка                     │
│  4.  Макс одновременных позиций        │
│  5.  Макс валовая экспозиция           │
│  6.  Макс номинал позиции              │
│  7.  Макс плечо                        │
│  8.  Макс проскальзывание              │
│  9.  Частота ордеров                   │
│  10. Подряд убыточные сделки           │
│  11. Актуальность данных               │
│  12. Качество стакана                  │
│  13. Ширина спреда                     │
│  14. Минимальная ликвидность           │
│                                         │
│  Verdict: Approved/Denied/ReduceSize/   │
│           Throttled                     │
└─────────────────────────────────────────┘
    │
    ▼
TWAP Executor (опционально)
    │  Триггер 1: exec_alpha.slice_plan.has_value() → нарезка по рекомендации
    │  Триггер 2: twap_executor_->should_use_twap() → независимая оценка TWAP
    │  ExecutionAlpha является первичным арбитром при конфликте триггеров
    │
    ▼
ExecutionEngine → OrderRecord + FSM
    │  Создаёт запись ордера
    │  Управляет FSM (конечный автомат состояний)
    │  Проверяет дублирование
    │  Отправляет через IOrderSubmitter
    │
    ▼
PortfolioEngine (обновление позиций)
    │  При заполнении: открытие/обновление позиции
    │  Отслеживание P&L, экспозиции, просадки
```

## ExecutionAlpha — архитектура и логика

### Стили исполнения

| Стиль | Условие | Fill Prob | Комиссия |
|-------|---------|-----------|----------|
| `PostOnly` | spread < 4.5 bps И urgency < 0.35 И adverse < 0.35 | 0.25–0.75 | 0 (maker rebate) |
| `Passive` | urgency < 0.5 И spread < 15 bps (с поправкой на имбаланс) | 0.25–0.75 | Maker |
| `Hybrid` | urgency 0.5–0.8 ИЛИ неблагоприятный имбаланс | 0.60–0.92 | Частично taker |
| `Aggressive` | urgency > 0.8 | 0.95 | Taker |
| `NoExecution` | spread > 50 bps ИЛИ VPIN_toxic ИЛИ adverse > 0.7 | 0.0 | — |

### Источники adverse selection (взвешенная агрегация)

```
adverse_score = Σ(factor × weight) / Σ(weight)

┌─────────────────────┬────────┬───────────────────────────────────────────────┐
│ Фактор              │ Вес    │ Источник                                      │
├─────────────────────┼────────┼───────────────────────────────────────────────┤
│ VPIN                │ 0.40   │ features.microstructure.vpin (если valid)     │
│ aggressive_flow     │ 1.0    │ features.microstructure.aggressive_flow       │
│ book_instability    │ 1.0    │ features.microstructure.book_instability      │
│ spread_bps / 100    │ 1.0    │ features.microstructure.spread_bps            │
└─────────────────────┴────────┴───────────────────────────────────────────────┘

+ Жёсткий стоп: если vpin_toxic=true AND vpin > vpin_toxic_threshold → NoExecution
  (применяется ДО взвешенного расчёта)
```

### Расчёт срочности

```
urgency = clamp(base + Δvol + Δmomentum + Δcusum + Δtod, 0.0, 1.0)

Δvol      = min(volatility_5 × 2.0, 0.20)        # Волатильность → спешить
Δmomentum = min(|momentum_5| × 0.5, 0.15)         # Сильный тренд → спешить
Δcusum    = 0.15 если cusum_regime_change = true   # Смена режима → срочно
Δtod      = tod_alpha_score × 0.10                 # Плохая сессия → -urgency
```

### DecisionFactors (аудит-трасса)

Каждое решение публикует `DecisionFactors` для полной прозрачности:

```cpp
struct DecisionFactors {
    // Adverse selection [0..1]: spread_toxicity, flow_toxicity,
    //   book_instability_score, vpin_toxicity, adverse_selection_score
    // Имбаланс: directional_imbalance [-1..+1], imbalance_used
    // Срочность: urgency_base, urgency_vol_adj, urgency_momentum_adj,
    //   urgency_cusum_adj, urgency_tod_adj
    // Качество данных: vpin_used, weighted_mid_used, features_complete
};
```

### Лимитная цена

```
mid = weighted_mid_price (если доступна и use_weighted_mid_price=true)
    | mid_price (fallback)

improvement = min(mid × limit_price_passive_bps/10000, half_spread × 0.30)

BUY  Passive/PostOnly: limit = mid - half_spread + improvement  (вблизи best bid)
SELL Passive/PostOnly: limit = mid + half_spread - improvement  (вблизи best ask)
BUY  Hybrid:           limit = mid - half_spread × 0.4          (внутри спреда)
SELL Hybrid:           limit = mid + half_spread × 0.4
```

### Интеграция с TWAP

`slice_plan` из `ExecutionAlphaResult` служит первичным триггером для TWAP executor.
Если ордер превышает `large_order_slice_threshold` (% от глубины стакана), execution_alpha
вычисляет оптимальное количество слайсов и интервал (адаптивный по нестабильности стакана).

```
TWAP trigger = exec_alpha.slice_plan.has_value()
             OR twap_executor_->should_use_twap(intent, snapshot)
```

## Специальные случаи (hardcoded overrides)

| Сценарий | Поведение |
|----------|-----------|
| **Stop-loss** | Обходит execution_alpha; жёстко Aggressive + should_execute=true |
| **Закрытие позиции** | Результат execution_alpha переопределяется → Aggressive |
| **TWAP slices** | Каждый слайс проходит execution_alpha независимо |

**КРИТИЧНО: overrides stop-loss НЕЛЬЗЯ изменять.** Они гарантируют, что аварийное
закрытие позиции не задерживается анализом рыночных условий.

## Конфигурация (`execution_alpha` секция)

```yaml
execution_alpha:
  max_spread_bps_passive: 15.0       # Макс спред для Passive [bps]
  max_spread_bps_any: 50.0           # Макс спред для любого исполнения [bps]
  adverse_selection_threshold: 0.7   # Порог NoExecution [0..1]
  urgency_passive_threshold: 0.5     # Ниже → Passive
  urgency_aggressive_threshold: 0.8  # Выше → Aggressive
  large_order_slice_threshold: 0.10  # % от глубины → нарезка
  vpin_toxic_threshold: 0.65         # VPIN жёсткий стоп
  vpin_weight: 0.40                  # Вес VPIN в adverse score
  imbalance_favorable_threshold: 0.30
  imbalance_unfavorable_threshold: 0.30
  use_weighted_mid_price: true
  limit_price_passive_bps: 3.0       # Улучшение к best bid/ask [bps]
  urgency_cusum_boost: 0.15          # Буст при CUSUM
  urgency_tod_weight: 0.10           # Вес time-of-day в urgency
  min_fill_probability_passive: 0.25
  postonly_spread_threshold_bps: 4.5 # Условие для PostOnly
  postonly_urgency_max: 0.35
  postonly_adverse_max: 0.35
```

## Зависимости модулей

```
tb_execution_alpha → tb_common, tb_features, tb_strategy, tb_logging, tb_clock, tb_metrics
tb_opportunity_cost → tb_common, tb_strategy, tb_execution_alpha, tb_logging, tb_clock
tb_portfolio → tb_common, tb_logging, tb_clock, tb_metrics
tb_portfolio_allocator → tb_common, tb_strategy, tb_portfolio, tb_logging
tb_risk → tb_common, tb_strategy, tb_features, tb_portfolio, tb_portfolio_allocator,
          tb_execution_alpha, tb_logging, tb_clock, tb_metrics
tb_execution → tb_common, tb_strategy, tb_risk, tb_execution_alpha, tb_portfolio,
               tb_logging, tb_clock, tb_metrics
```

## Принципы проектирования

1. **Безопасность**: Никакой живой ордер не может обойти RiskEngine
2. **Прозрачность**: `DecisionFactors` фиксирует каждый фактор решения для аудита
3. **Потокобезопасность**: Все движки используют мьютексы для защиты состояния
4. **Тестируемость**: Все зависимости — через интерфейсы (DI)
5. **RAII**: Используется std::lock_guard для управления мьютексами
6. **Result-based errors**: Используется std::expected вместо исключений
7. **Конфигурируемость**: Все пороги вынесены в `execution_alpha` секцию YAML


## Обзор

Фаза 4 реализует полный путь от торгового решения до исполнения ордера.
Каждый шаг обязателен, ни один торговый путь не может обойти Риск-движок.

## Конвейер исполнения

```
DecisionRecord (из decision/)
    │
    ▼
ExecutionAlphaEngine → ExecutionAlphaResult
    │  Определяет стиль исполнения (Passive/Aggressive/Hybrid)
    │  Оценивает качество исполнения (спред, проскальзывание)
    │  Рассчитывает лимитную цену и план нарезки
    │
    ▼
OpportunityCostEngine → OpportunityCostResult
    │  Оценивает ожидаемый доход vs стоимость исполнения
    │  Решает: Execute / Defer / Suppress
    │
    ▼
PortfolioAllocator → SizingResult
    │  Применяет иерархию бюджетов
    │  Лимит концентрации (≤20% капитала на одну позицию)
    │  Лимит стратегии (≤30% капитала)
    │  Множитель неопределённости
    │  Ограничение доступным капиталом
    │
    ▼
┌─────────────────────────────────────────┐
│     RiskEngine → RiskDecision           │
│     *** ОБЯЗАТЕЛЬНЫЙ, ОБХОД ЗАПРЕЩЁН ***│
│                                         │
│  14 правил последовательной проверки:   │
│  1.  Kill switch                        │
│  2.  Макс дневной убыток              │
│  3.  Макс просадка                     │
│  4.  Макс одновременных позиций        │
│  5.  Макс валовая экспозиция           │
│  6.  Макс номинал позиции              │
│  7.  Макс плечо                        │
│  8.  Макс проскальзывание              │
│  9.  Частота ордеров                   │
│  10. Подряд убыточные сделки           │
│  11. Актуальность данных               │
│  12. Качество стакана                  │
│  13. Ширина спреда                     │
│  14. Минимальная ликвидность           │
│                                         │
│  Verdict: Approved/Denied/ReduceSize/   │
│           Throttled                     │
└─────────────────────────────────────────┘
    │
    ▼
ExecutionEngine → OrderRecord + FSM
    │  Создаёт запись ордера
    │  Управляет FSM (конечный автомат состояний)
    │  Проверяет дублирование
    │  Отправляет через IOrderSubmitter
    │
    ▼
PortfolioEngine (обновление позиций)
    │  При заполнении: открытие/обновление позиции
    │  Отслеживание P&L, экспозиции, просадки
```

## Зависимости модулей

```
tb_execution_alpha → tb_common, tb_features, tb_strategy, tb_logging, tb_clock, tb_metrics
tb_opportunity_cost → tb_common, tb_strategy, tb_execution_alpha, tb_logging, tb_clock
tb_portfolio → tb_common, tb_logging, tb_clock, tb_metrics
tb_portfolio_allocator → tb_common, tb_strategy, tb_portfolio, tb_logging
tb_risk → tb_common, tb_strategy, tb_features, tb_portfolio, tb_portfolio_allocator,
          tb_execution_alpha, tb_logging, tb_clock, tb_metrics
tb_execution → tb_common, tb_strategy, tb_risk, tb_execution_alpha, tb_portfolio,
               tb_logging, tb_clock, tb_metrics
```

## Принципы проектирования

1. **Безопасность**: Никакой живой ордер не может обойти RiskEngine
2. **Потокобезопасность**: Все движки используют мьютексы для защиты состояния
3. **Тестируемость**: Все зависимости — через интерфейсы (DI)
4. **RAII**: Используется std::lock_guard для управления мьютексами
5. **Result-based errors**: Используется std::expected вместо исключений
