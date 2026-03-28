# World Model v2.0 — Профессиональная классификация рыночного состояния

## Обзор

World Model v2.0 — центральный state/belief модуль торговой системы Tomorrow Bot.
Классифицирует текущее рыночное состояние по 9 дискретным состояниям с вероятностной
оценкой, explainability, обратной связью и конфигурируемыми порогами.

**Зачем для спотовой торговли:**
- Рынок криптовалют переключается между режимами за секунды
- Торговля в неподходящем состоянии ведёт к убыткам от проскальзывания, ложных пробоев, toxic flow
- Нужен stateful модуль с историей, вероятностями и обратной связью, а не snapshot-only эвристика
- Downstream-модули (strategy allocator, uncertainty, decision, execution) получают confidence-aware контекст

**Ключевые характеристики v2:**
- 9 состояний рынка с конфигурируемыми порогами классификации
- Гистерезис: подтверждение перехода несколькими тиками (с bypass для опасных состояний)
- Per-symbol history buffer с transition matrix и empirical persistence
- Composite fragility: 6 компонентов вместо линейной базы
- Вероятностная уверенность (confidence) по 4 факторам
- State probabilities (вероятностное распределение по состояниям)
- Многомерная suitability: signal × execution × risk с hard veto
- Explainability: triggered/checked conditions, top drivers, summary
- Feedback loop: EMA-обновление пригодности по фактическим P&L исходам
- Полная обратная совместимость с v1 downstream-модулями

---

## Архитектура

### Интерфейс IWorldModelEngine

```cpp
class IWorldModelEngine {
public:
    // Основной метод — классификация состояния по snapshot фич
    virtual WorldModelSnapshot update(const FeatureSnapshot& snapshot) = 0;

    // Кэшированное состояние по символу
    virtual std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const = 0;

    // v2: запись обратной связи от PnL
    virtual void record_feedback(const WorldStateFeedback& feedback) {}

    // v2: статистика эффективности по состояниям
    virtual std::optional<StatePerformanceStats> performance_stats(
        WorldState state, const StrategyId& strategy) const { return std::nullopt; }

    // v2: версия модели
    virtual std::string model_version() const { return "1.0.0"; }
};
```

### Файловая структура

```
src/world_model/
├── world_model_types.hpp       # Типы: WorldState, WorldModelSnapshot, Explanation, Feedback
├── world_model_config.hpp      # Конфигурация: пороги, параметры гистерезиса, feedback
├── world_model_history.hpp     # Per-symbol контекст: history buffer, transition matrix
├── world_model_engine.hpp      # Интерфейс + декларация RuleBasedWorldModelEngine
├── world_model_engine.cpp      # Полная реализация (~900 строк)
└── CMakeLists.txt              # Сборка модуля
```

---

## Состояния рынка

| # | Состояние | Описание | Опасность |
|---|-----------|----------|-----------|
| 0 | StableTrend | Устойчивый тренд с ADX > 25 | Низкая |
| 1 | ChopNoise | Боковое движение без направления | Низкая |
| 2 | FragileBreakout | Пробой BB с высокой волатильностью | Средняя |
| 3 | CompressionBeforeExpansion | Сжатие BB перед расширением | Средняя |
| 4 | ExhaustionSpike | RSI экстремум + momentum | Высокая |
| 5 | LiquidityVacuum | Критический спред, исчезновение ликвидности | Критическая |
| 6 | ToxicMicrostructure | Toxic flow + book instability | Критическая |
| 7 | PostShockStabilization | Восстановление после шока | Средняя |
| 8 | Unknown | Недостаточно данных | — |

### Приоритет классификации

Правила проверяются в порядке приоритета (первое сработавшее побеждает):

1. **PostShockStabilization** — только если предыдущее состояние было ExhaustionSpike/LiquidityVacuum
2. **ToxicMicrostructure** — book_instability > 0.7 И aggressive_flow > 0.8 И spread > 15 bps
3. **LiquidityVacuum** — spread > 50 bps ИЛИ (spread > 20 bps И liquidity_ratio < 0.3)
4. **ExhaustionSpike** — RSI > 80 или < 20 И |momentum| > 0.02
5. **FragileBreakout** — BB %B > 0.95 или < 0.05 И volatility > 0.02 И |imbalance| > 0.3
6. **CompressionBeforeExpansion** — BB bandwidth < 0.03 И ATR < 0.01 И volatility < 0.01
7. **StableTrend** — ADX > 25 И RSI 40–70
8. **ChopNoise** — ADX < 20 И RSI 40–60 И spread < 20 bps
9. **Unknown** — fallback

Все пороги вынесены в `WorldModelConfig` и конфигурируются через YAML.

---

## Гистерезис

Предотвращает шумовое переключение между состояниями:

- **confirmation_ticks** (default: 2) — новое состояние должно подтвердиться N тиков подряд
- **min_dwell_ticks** (default: 3) — минимальное время пребывания в текущем состоянии
- **min_confidence_to_switch** (default: 0.55) — минимальная уверенность для перехода

### Bypass (немедленные переходы без гистерезиса)

| Состояние | Условие bypass |
|-----------|---------------|
| LiquidityVacuum | Всегда (критическая опасность) |
| ToxicMicrostructure | Всегда (критическая опасность) |
| ExhaustionSpike | Всегда (высокая опасность) |
| PostShockStabilization | При переходе из ExhaustionSpike/LiquidityVacuum |

---

## Composite Fragility

Хрупкость вычисляется как взвешенная сумма 6 компонентов:

```
fragility = Σ(weight_i × component_i)  ∈ [0.0, 1.0]
```

| Компонент | Вес (default) | Источник |
|-----------|--------------|----------|
| Spread stress | 0.15 | spread_bps / 100 |
| Book instability | 0.20 | microstructure.book_instability |
| Volatility acceleration | 0.12 | vol_5 / vol_20 |
| Liquidity imbalance | 0.10 | |book_imbalance| |
| Transition instability | 0.15 | Частота переходов в окне |
| VPIN toxicity | 0.10 | microstructure.vpin_toxicity |

При отсутствии данных компонент заменяется нейтральным значением (0.5).

---

## Confidence (уверенность классификации)

Вычисляется по 4 факторам:

1. **Classification proximity** — насколько убедительно сработало правило (0.0–1.0)
2. **Data quality** — доля валидных индикаторов из необходимых
3. **Persistence agreement** — совпадение с эмпирической персистентностью
4. **Transition stability** — стабильность текущего состояния в последних N тиках

```
confidence = 0.4 × proximity + 0.25 × data_quality + 0.2 × persistence + 0.15 × transition
```

---

## State Probabilities

Вероятностное распределение по всем 9 состояниям:

```
P(state_i) = α × confidence_indicator + β × uniform_prior + γ × transition_matrix
```

- `confidence_indicator`: 1.0 для текущего состояния × confidence, остальные распределены равномерно
- `transition_matrix`: эмпирическая матрица переходов из истории
- Нормализация: Σ P(state_i) = 1.0

---

## Многомерная Strategy Suitability

Пригодность стратегии оценивается по 3 измерениям:

| Измерение | Что оценивает |
|-----------|--------------|
| Signal suitability | Качество сигнала в текущем состоянии |
| Execution suitability | Исполнимость (спреды, ликвидность, toxic flow) |
| Risk suitability | Соответствие рисковым лимитам |

```
suitability = signal × execution × risk
```

### Hard Veto

Стратегия получает `vetoed = true` при `suitability < hard_veto_threshold` (default: 0.05).
Вето-стратегии не участвуют в аллокации независимо от других факторов.

### Feedback Blend

При наличии достаточной статистики (> `min_trades_for_feedback`):

```
suitability_final = (1 - α) × suitability_base + α × feedback_adjustment
feedback_adjustment = f(win_rate, avg_pnl)  ∈ [0.5, 1.5]
```

---

## Explainability

Каждый snapshot содержит `WorldModelExplanation`:

```cpp
struct WorldModelExplanation {
    std::vector<ClassificationCondition> triggered_conditions;  // Сработавшие правила
    std::vector<ClassificationCondition> checked_conditions;    // Все проверенные правила
    std::vector<std::string> top_drivers;                       // Топ факторов (текстовые)
    std::string summary;                                        // Человекочитаемое описание
    double data_quality;                                        // Качество входных данных
    bool hysteresis_overrode;                                   // Гистерезис заблокировал переход?
    int confirmation_ticks_remaining;                           // Тиков до подтверждения
};
```

Пример summary:
```
[StableTrend] conf=0.82 frag=0.15 dwell=42 | ADX=32.5 RSI=55.3 | data_quality=0.90
```

---

## Feedback Loop

Позволяет адаптировать suitability на основе фактических торговых результатов:

1. **record_feedback()** — записывает P&L исход по (state, strategy)
2. **EMA update** — скользящее среднее win_rate и avg_pnl с `ema_alpha` (default: 0.05)
3. **Suitability adjustment** — при `trade_count >= min_trades_for_feedback`
4. **Guardrails** — adjustment ∈ [0.5, 1.5], предотвращает чрезмерную адаптацию

---

## Transition Context

```cpp
struct TransitionContext {
    WorldState previous_state;
    TransitionTendency tendency;      // Improving / Degrading / Stable
    double transition_velocity;       // Скорость смены состояний
    double regime_alignment;          // Согласованность с RegimeEngine
    int ticks_since_last_transition;
};
```

`tendency` определяется сравнением quality score текущего и предыдущего состояния:
- StableTrend = +3 (лучшее)
- CompressionBeforeExpansion = +2
- PostShockStabilization = +2
- ChopNoise = +1
- FragileBreakout = 0
- ExhaustionSpike = -1
- ToxicMicrostructure = -2
- LiquidityVacuum = -2
- Unknown = 0

---

## Конфигурация

Вся конфигурация вынесена в `WorldModelConfig` с `make_default()` и `validate()`:

```yaml
world_model:
  model_version: "2.0.0"
  min_valid_indicators: 2

  # Пороги классификации (по состояниям)
  toxic_microstructure:
    book_instability_min: 0.7
    ...

  # Composite fragility weights
  fragility:
    spread_stress_weight: 0.15
    ...

  # Гистерезис
  hysteresis:
    enabled: true
    confirmation_ticks: 2
    ...

  # Feedback loop
  feedback:
    enabled: true
    ema_alpha: 0.05
    ...
```

Конфигурация добавлена во все профили: `production.yaml`, `paper.yaml`, `shadow.yaml`, `testnet.yaml`.

---

## Downstream-интеграция

### Pipeline (trading_pipeline.cpp)
```cpp
auto world = world_model_->update(snapshot);
// world содержит: state, label, confidence, fragility, suitability, explanation, ...
```

### Strategy Allocator
Использует `world.strategy_suitability[i].suitability` для взвешивания стратегий.
v2 добавляет `vetoed` flag для hard stop, `signal/execution/risk_suitability` для диагностики.

### Uncertainty Engine
Потребляет полный `WorldModelSnapshot` для оценки неопределённости среды.
v2 confidence и fragility улучшают точность uncertainty scoring.

### Decision Aggregation
Записывает `world.state` как `world_state` в trade record для аудита.

---

## Обратная совместимость

- Все поля v1 `WorldModelSnapshot` сохранены (state, label, fragility, tendency, persistence_score, strategy_suitability, computed_at, symbol)
- v2 поля добавлены как расширения: confidence, state_probabilities, transition, explanation, model_version, dwell_ticks
- v2 методы интерфейса имеют default implementation (no-op)
- v1-совместимый конструктор `RuleBasedWorldModelEngine(logger, clock)` создаёт engine с default config
- Downstream-модули не требуют изменений для работы с v2

---

## Тестирование

28 тестов покрывают:

| Категория | Тесты |
|-----------|-------|
| Классификация (все 9 состояний) | 9 |
| Метки (to_label) | 1 |
| Кэширование (current_state) | 1 |
| Multi-dimension suitability | 2 |
| Конфигурируемые пороги | 1 |
| Explainability | 2 |
| Гистерезис | 2 |
| Dwell ticks | 1 |
| Persistence blend | 1 |
| State probabilities | 1 |
| Feedback loop | 1 |
| Model version | 1 |
| Config validation | 1 |
| Multi-symbol isolation | 1 |
| Transition tendency | 1 |
| Composite fragility | 1 |
| Data quality | 1 |

---

## Будущие направления (P1–P3)

### P1: Fusion с RegimeEngine
- Общий belief state: regime probabilities + world probabilities
- Единый confidence-aware контекст для всех downstream-модулей

### P2: ML-гибридизация
- Интеграция entropy_filter как quality gate
- Bayesian adapter для обновления transition matrix
- Correlation monitor как state modifier

### P3: Latent State Engine
- HMM / Bayesian regime switching
- Ensemble inference
- Drift detection и автоматическая перекалибровка
