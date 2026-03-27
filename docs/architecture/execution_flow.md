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
    │  Оценивает opportunity cost на уровне сигнала, портфеля и исполнения
    │  Нелинейный скоринг: conviction^1.5 × bps_scale − execution_cost
    │  10 каскадных правил приоритезации с адаптивными порогами
    │  Портфельный контекст: концентрация, экспозиция, просадка, серии убытков
    │  Решает: Execute / Defer / Suppress / Upgrade
    │  Полная аудит-трасса: OpportunityCostFactors + rule_id + counterfactual
    │  Метрики: Prometheus counters/gauges/histogram
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

## OpportunityCost — архитектура и логика

### Модель скоринга

```
expected_return_bps = conviction^1.5 × conviction_to_bps_scale
net_expected_bps    = expected_return_bps − execution_cost_bps
capital_efficiency  = net_expected_bps / max(exposure, 0.05)

composite_score = w_conviction × conviction_component
                + w_net_edge   × net_edge_component
                + w_capital    × capital_efficiency_component
                + w_urgency    × urgency_component

Все веса конфигурируемы, сумма = 1.0
```

### Адаптивный порог conviction

```
effective_threshold = base_threshold
                    + drawdown_penalty_scale × (drawdown% / 5%)
                    + 0.02 × consecutive_losses

Клэмп: [0.0, 0.95]
```

При просадке и серии убытков порог автоматически повышается,
система становится консервативнее.

### 10 каскадных правил приоритезации

| # | Правило | Действие |
|---|---------|----------|
| 1 | net_edge < min_net_expected_bps | Suppress (NegativeNetEdge) |
| 2 | conviction < effective_threshold | Suppress (ConvictionBelowThreshold) |
| 3 | exposure > capital_exhaustion_pct | Suppress (CapitalExhausted) |
| 4 | symbol concentration > max_symbol_concentration | Suppress (SymbolConcentrationExceeded) |
| 5 | strategy concentration > max_strategy_concentration | Defer (StrategyConcentrationExceeded) |
| 6 | exposure > high_exposure_threshold И conviction < 0.7 | Defer (HighExposureLowConviction) |
| 7 | net_edge лучше worst position + min_edge_advantage | Upgrade (UpgradeOpportunity) |
| 8 | net_edge ≥ min_net_expected_bps И exposure < capital_exhaustion | Execute (StrongEdgeAvailable) |
| 9 | conviction ≥ high_conviction_override | Execute (HighConvictionOverride) |
| 10 | Остальное | Defer (InsufficientNetEdge) |

Правила каскадные — первое сработавшее определяет действие.

### Портфельный контекст (PortfolioContext)

```cpp
struct PortfolioContext {
    double gross_exposure_pct;         // Текущая грубая экспозиция [0..1]
    double net_exposure_pct;           // Чистая экспозиция [−1..+1]
    double available_capital_pct;      // Доступный капитал [0..1]
    double current_drawdown_pct;       // Текущая просадка [0..1]
    int    active_positions;           // Число активных позиций
    int    consecutive_losses;         // Подряд убыточные сделки
    double symbol_concentration;       // Концентрация по символу [0..1]
    double strategy_concentration;     // Концентрация по стратегии [0..1]
    double worst_position_edge_bps;    // Edge худшей позиции
    double portfolio_heat;             // Суммарный PnL-heat [0..1]
    double recent_win_rate;            // Winrate за окно
    double avg_holding_period_minutes; // Средний срок удержания
};
```

Контекст конструируется из `PortfolioSnapshot` непосредственно перед вызовом.

### Аудит-трасса (OpportunityCostFactors)

```cpp
struct OpportunityCostFactors {
    int    rule_id;                    // Номер сработавшего правила
    double raw_conviction;             // Входная conviction
    double effective_conviction_threshold; // Адаптивный порог
    double expected_return_bps;        // Ожидаемый доход
    double execution_cost_bps;         // Стоимость исполнения
    double net_expected_bps;           // Чистый edge
    double capital_efficiency;         // Эффективность капитала
    double composite_score;            // Composite score [0..1]
    OpportunityAction counterfactual_action; // Что было бы при пороге −20%
    std::string decision_rationale;    // Человекочитаемое объяснение
};
```

### Специальные случаи

| Сценарий | Поведение |
|----------|-----------|
| **Закрытие позиции** | Обходит OpportunityCostEngine (безусловное исполнение) |
| **Upgrade** | Детектируется, но текущая версия пропускает как Execute (placeholder для phase 3) |

### Конфигурация (`opportunity_cost` секция)

```yaml
opportunity_cost:
  conviction_to_bps_scale: 150.0    # Масштаб conviction → bps
  min_conviction_threshold: 0.25    # Базовый порог conviction [0..1]
  min_net_expected_bps: 2.0         # Мин чистый edge для Execute [bps]
  max_exposure_for_new_entry: 0.85  # Макс экспозиция для новых входов
  high_conviction_override: 0.85    # Conviction для override → Execute
  min_edge_advantage_bps: 5.0       # Мин преимущество для Upgrade [bps]
  capital_exhaustion_pct: 0.90      # Порог исчерпания капитала
  high_exposure_threshold: 0.70     # Порог высокой экспозиции
  max_symbol_concentration: 0.25    # Макс концентрация по символу
  max_strategy_concentration: 0.35  # Макс концентрация по стратегии
  drawdown_penalty_scale: 0.10      # Масштаб штрафа за просадку
  weight_conviction: 0.35           # Вес conviction в composite score
  weight_net_edge: 0.30             # Вес net edge
  weight_capital_efficiency: 0.20   # Вес capital efficiency
  weight_urgency: 0.15              # Вес urgency (сумма весов = 1.0)
```

```
tb_execution_alpha → tb_common, tb_features, tb_strategy, tb_logging, tb_clock, tb_metrics
tb_opportunity_cost → tb_common, tb_strategy, tb_execution_alpha, tb_logging, tb_clock, tb_metrics
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
7. **Конфигурируемость**: Все пороги вынесены в YAML секции (`execution_alpha`, `opportunity_cost`)
