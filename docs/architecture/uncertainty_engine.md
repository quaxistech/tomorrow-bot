# Uncertainty Engine v2

## Обзор

Uncertainty Engine v2 — модуль оценки неопределённости торговой среды по 9 измерениям.
Предоставляет единый агрегированный скор и рекомендации для принятия решений:
размер позиции, пороги входа, стиль исполнения, cooldown.

**Зачем для спотовой торговли:**
- Рынок криптовалют характеризуется высокой нестабильностью режимов, ликвидности и микроструктуры
- Статичные пороги приводят к избыточной торговле в неопределённых условиях
- Необходима адаптация: от уменьшения размера до полной остановки торговли
- Интеграция с risk engine, execution alpha и execution engine обеспечивает сквозную защиту

**Ключевые характеристики v2:**
- 9 измерений (было 4+1) — полное покрытие источников неопределённости
- Stateful: EMA-сглаживание, гистерезис, shock memory, cooldown
- ML-aware: потребляет signal quality, cascade probability, correlation breaks
- Portfolio-aware: концентрация, exposure, drawdown
- Нелинейная агрегация с regime-specific amplifiers
- Production observability: top drivers, execution mode, диагностика
- 25 тестов покрывают все измерения и stateful поведение

---

## Архитектура

### Интерфейс IUncertaintyEngine

```cpp
class IUncertaintyEngine {
public:
    // v1 (обратная совместимость) — 3 аргумента
    virtual UncertaintySnapshot assess(
        const FeatureSnapshot& features,
        const RegimeSnapshot& regime,
        const WorldModelSnapshot& world) = 0;

    // v2 (полный контекст) — 5 аргументов
    virtual UncertaintySnapshot assess(
        const FeatureSnapshot& features,
        const RegimeSnapshot& regime,
        const WorldModelSnapshot& world,
        const PortfolioSnapshot& portfolio,
        const MlSignalSnapshot& ml_signals) = 0;

    virtual std::optional<UncertaintySnapshot> current(const Symbol& symbol) const = 0;
    virtual UncertaintyDiagnostics diagnostics() const = 0;
    virtual void record_feedback(const UncertaintyFeedback& feedback) = 0;
    virtual void reset_state() = 0;
};
```

- `assess()` — два перегрузки: v1 для обратной совместимости (portfolio/ML defaults), v2 с полным контекстом
- `current()` — последний кэшированный снапшот для символа
- `diagnostics()` — метрики здоровья модуля
- `record_feedback()` — обратная связь по результатам сделок
- `reset_state()` — сброс всего внутреннего состояния

### Реализация RuleBasedUncertaintyEngine

Rule-based реализация с 9 компьютерами измерений, stateful агрегацией и thread-safe
хранением per-symbol состояния.

```
FeatureSnapshot ──┐
RegimeSnapshot  ──┤
WorldModelSnapshot┤
PortfolioSnapshot ┤──→ RuleBasedUncertaintyEngine
MlSignalSnapshot ─┘         │
                            ├─→ compute_regime_uncertainty()
                            ├─→ compute_signal_uncertainty()
                            ├─→ compute_data_quality_uncertainty()
                            ├─→ compute_execution_uncertainty()
                            ├─→ compute_portfolio_uncertainty()
                            ├─→ compute_ml_uncertainty()
                            ├─→ compute_correlation_uncertainty()
                            ├─→ compute_transition_uncertainty()
                            ├─→ compute_operational_uncertainty()
                            │
                            ▼
                      aggregate() — взвешенная сумма
                            + regime amplifiers
                            + tail stress
                            │
                            ▼
                      apply_hysteresis() → UncertaintyLevel
                      update_state() → EMA, shock memory
                      determine_execution_mode()
                      compute_drivers() → top-3
                            │
                            ▼
                      UncertaintySnapshot
```

### Модель агрегации

1. **Взвешенная сумма** — каждое измерение × его вес
2. **Regime-specific amplifiers:**
   - Volatile режим: data_quality и execution ×1.5, signal ×0.8
   - Unclear режим: все измерения ×1.2
3. **Tail stress:** +0.05 за каждое измерение > 0.8 (нелинейная штрафная функция)
4. Результат зажат в [0, 1]

---

## Измерения неопределённости

| # | Измерение | Вес | Источник | Описание |
|---|-----------|-----|----------|----------|
| 1 | **Regime** | 0.25 | RegimeSnapshot | `1.0 - confidence`. Низкая уверенность в классификации режима → высокая неопределённость |
| 2 | **Signal** | 0.20 | FeatureSnapshot | Конфликты индикаторов: RSI vs MACD, расхождение EMA. Штраф при < 3 валидных индикаторах |
| 3 | **Data Quality** | 0.10 | FeatureSnapshot | Качество данных: stale data, невалидный стакан, широкий спред (>30 bps), отсутствие SMA |
| 4 | **Execution** | 0.10 | FeatureSnapshot | Спред (bps), расчётное проскальзывание, коэффициент ликвидности |
| 5 | **Portfolio** | 0.10 | PortfolioSnapshot | Концентрация (single pos > 40–60% капитала), утилизация >70–90%, drawdown, кол-во позиций |
| 6 | **ML** | 0.10 | MlSignalSnapshot | `1.0 - signal_quality` + штрафы за cascade; 0.7 при degraded/stale ML |
| 7 | **Correlation** | 0.05 | MlSignalSnapshot | 0.8 при correlation break; иначе `1.0 - correlation_risk_multiplier` |
| 8 | **Transition** | 0.05 | RegimeSnapshot | Transition confidence + штраф при stability < 0.3 |
| 9 | **Operational** | 0.05 | FeatureSnapshot | Feed freshness, book quality, infrastructure health. 0.7 при stale feed |

---

## Конфигурация

### UncertaintyConfig

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `w_regime` | double | 0.25 | Вес измерения Regime |
| `w_signal` | double | 0.20 | Вес измерения Signal |
| `w_data_quality` | double | 0.10 | Вес измерения Data Quality |
| `w_execution` | double | 0.10 | Вес измерения Execution |
| `w_portfolio` | double | 0.10 | Вес измерения Portfolio |
| `w_ml` | double | 0.10 | Вес измерения ML |
| `w_correlation` | double | 0.05 | Вес измерения Correlation |
| `w_transition` | double | 0.05 | Вес измерения Transition |
| `w_operational` | double | 0.05 | Вес измерения Operational |
| `threshold_low` | double | 0.25 | Граница Low → Moderate |
| `threshold_moderate` | double | 0.50 | Граница Moderate → High |
| `threshold_high` | double | 0.75 | Граница High → Extreme |
| `hysteresis_up` | double | 0.03 | Буфер для повышения уровня |
| `hysteresis_down` | double | 0.05 | Буфер для понижения уровня |
| `ema_alpha` | double | 0.15 | Коэффициент EMA для persistent_score [0,1] |
| `size_floor` | double | 0.10 | Минимальный size_multiplier (никогда 0) |
| `threshold_ceiling` | double | 3.0 | Максимальный threshold_multiplier |
| `calibration_decay` | double | 0.995 | Экспоненциальный decay калибровочной уверенности |
| `min_feedback_samples` | uint32_t | 50 | Минимум сэмплов для калибровки |

---

## Интеграции

### Pipeline: поток оценки

```
FeatureEngine → features
RegimeEngine → regime
WorldModel → world
Portfolio → portfolio_snapshot    ──→ UncertaintyEngine.assess()
ML modules → ml_signal_snapshot         │
                                        ▼
                                  UncertaintySnapshot
                                        │
              ┌─────────────────────────┬┴──────────────┐
              ▼                         ▼               ▼
        StrategyAllocator         DecisionEngine   ExecutionAlpha
        (size_multiplier)    (threshold_multiplier   (style/urgency)
                              + veto при NoTrade)
              │                         │               │
              ▼                         ▼               ▼
        PortfolioAllocator         RiskEngine      ExecutionEngine
        (sizing)              (3 uncertainty-aware   (guards)
                                   проверки)
```

### Risk Engine — 3 uncertainty-aware проверки

| Проверка | Описание |
|----------|----------|
| **check_uncertainty_limits** | Ограничения позиции на основе uncertainty level. Extreme → блокировка |
| **check_uncertainty_cooldown** | Применяет cooldown decay factor. После серии Extreme — принудительная пауза |
| **check_uncertainty_execution_mode** | Проверяет execution mode recommendation. HaltNewEntries → вето |

### Execution Alpha

UncertaintySnapshot влияет на:
- **Стиль исполнения** — при высокой неопределённости переход к пассивному стилю
- **Urgency** — снижение urgency при uncertain conditions
- **Fill probability targets** — более консервативные цели
- **Spread/toxicity adjustments** — учёт текущей микроструктуры

### Execution Engine

- `size_multiplier` применяется к размеру ордера
- `threshold_multiplier` влияет на цену входа
- Cooldown decay factor ограничивает частоту ордеров
- Uncertainty metadata записывается в OrderRecord для аудита

### Strategy Allocator

```
weight = base_weight
       × regime_hint.weight_multiplier
       × world.suitability
       × uncertainty.size_multiplier    ← uncertainty v2
```

### Decision Engine

- `effective_threshold = base_threshold × uncertainty.threshold_multiplier × regime_factor`
- `UncertaintyAction::NoTrade` → глобальное вето (rejection reason: GlobalUncertaintyVeto)

### Portfolio Allocator

- `size_multiplier` из UncertaintySnapshot масштабирует итоговый размер позиции
- Гарантия: size_multiplier ≥ `size_floor` (default 0.10) — позиция никогда не обнуляется

---

## Stateful модель

### EMA-сглаживание

```
persistent_score = ema_alpha × instant_score + (1 - ema_alpha) × prev_persistent_score
```

- `ema_alpha = 0.15` по умолчанию
- Фильтрует кратковременный шум
- `persistent_score` используется для уровня (Low/Moderate/High/Extreme)
- `spike_score = instant_score - persistent_score` — детекция резких скачков

### Гистерезис (Hysteresis Bands)

Предотвращает осцилляцию между уровнями:

```
Для повышения уровня:  score > threshold + hysteresis_up  (0.03)
Для понижения уровня:  score < threshold - hysteresis_down (0.05)
```

Пример: при `threshold_moderate = 0.50`:
- Переход Low → Moderate: score > 0.53
- Возврат Moderate → Low: score < 0.45

### Shock Memory

Per-symbol отслеживание серий экстремальных оценок:
- Счётчик consecutive Extreme assessments
- При 3+ подряд → активация cooldown
- Учитывается в `CooldownRecommendation`

### Cooldown

```cpp
struct CooldownRecommendation {
    bool active;
    double decay_factor;     // 0.0–1.0, применяется к sizing
    Timestamp expires_at;
};
```

- Активируется после серии Extreme оценок
- `decay_factor` постепенно возвращается к 1.0
- Интегрирован в Risk Engine (`check_uncertainty_cooldown`)

### Per-symbol tracking

- `std::unordered_map<Symbol, SymbolState>` — состояние для каждого инструмента
- Thread-safe: `std::mutex` + `std::lock_guard`
- `SymbolState` хранит: prev_level, prev_persistent_score, consecutive_extreme_count, cooldown

---

## Observability

### UncertaintyDiagnostics

| Поле | Тип | Описание |
|------|-----|----------|
| `total_assessments` | uint64_t | Общее количество вызовов assess() |
| `veto_count` | uint64_t | Количество вето NoTrade |
| `cooldown_activations` | uint64_t | Активации cooldown |
| `avg_assessment_latency_us` | double | Средняя латентность assess() (мкс) |
| `max_assessment_latency_us` | double | Пиковая латентность assess() (мкс) |
| `avg_aggregate_score` | double | Скользящее среднее aggregate_score |
| `calibration_error` | double | Brier score калибровочной ошибки [0,1] |
| `feedback_samples` | uint32_t | Количество feedback сэмплов |
| `active_model_version` | uint32_t | Версия активной модели (= 1) |
| `last_assessment` | Timestamp | Время последнего assess() |
| `last_feedback` | Timestamp | Время последнего feedback |

### Top Drivers

Каждый UncertaintySnapshot содержит top-3 drivers — измерения, вносящие наибольший вклад:

```cpp
struct UncertaintyDriver {
    std::string dimension;   // "regime", "signal", ...
    double score;            // Скор измерения [0,1]
    std::string label;       // Человекочитаемое описание
};
```

Позволяет быстро понять **почему** неопределённость высока.

### Execution Mode Recommendations

| Режим | Условие | Описание |
|-------|---------|----------|
| `Normal` | score < 0.25 | Стандартная торговля |
| `Conservative` | 0.25 ≤ score < 0.50 | Уменьшенные размеры, повышенные пороги |
| `DefensiveOnly` | 0.50 ≤ score < 0.75 | Только защитные действия (close/reduce) |
| `HaltNewEntries` | score ≥ 0.75 | Запрет новых входов |

### Телеметрия

Поля uncertainty v2 в TelemetryEnvelope:

| Поле | Описание |
|------|----------|
| `uncertainty_score` | Мгновенный агрегированный скор |
| `uncertainty_persistent_score` | EMA-сглаженный скор |
| `uncertainty_spike_score` | Разница instant - persistent |
| `uncertainty_execution_mode` | Рекомендованный режим исполнения |
| `uncertainty_calibration_confidence` | Доверие к калибровке |
| `uncertainty_top_drivers` | JSON top-3 drivers |

---

## Калибровка и обратная связь

### UncertaintyFeedback

```cpp
struct UncertaintyFeedback {
    Symbol symbol;
    Timestamp trade_time;
    double predicted_uncertainty;      // Предсказанная неопределённость
    double realized_slippage;          // Фактическое проскальзывание (%)
    double realized_pnl;              // Реализованный P&L
    double realized_volatility;        // Реализованная волатильность
    bool was_stopped_out;             // Сработал ли стоп-лосс
    UncertaintyAction action_taken;   // Применённое действие
    std::string notes;
};
```

### record_feedback()

- Записывает результат сделки в feedback buffer
- Обновляет `calibration_confidence` (экспоненциальный decay)
- Обновляет `calibration_error` (Brier score)
- Увеличивает `feedback_samples` в диагностике

### Будущее: replay-based calibration

Планируется использование replay engine для:
- Offline прогон исторических данных через uncertainty engine
- Сравнение predicted vs realized uncertainty
- Оптимизация весов измерений на основе P&L outcomes
- Требует накопления ≥ `min_feedback_samples` (50) для статистической значимости

---

## Тестирование

25 тестов в `tests/unit/uncertainty/uncertainty_test.cpp`:

### V1 тесты (обратная совместимость)

| # | Тест | Проверяет |
|---|------|-----------|
| 1 | Low uncertainty scenario | Стабильные условия → Low level, size_mult ≈ 1.0 |
| 2 | High uncertainty scenario | Нестабильные условия → High level |
| 3 | size_multiplier decreases | Рост uncertainty → снижение size_multiplier |
| 4 | threshold_multiplier increases | Рост uncertainty → рост threshold_multiplier |
| 5 | Extreme → NoTrade | Экстремальная неопределённость → вето |

### V2 тесты (полный контекст)

| # | Тест | Проверяет |
|---|------|-----------|
| 6 | 5-arg assess richer output | v2 overload возвращает расширенный снапшот |
| 7 | Concentrated portfolio | Высокая концентрация → высокая portfolio uncertainty |
| 8 | Empty portfolio | Пустой портфель → нулевая portfolio uncertainty |
| 9 | Degraded ML | Низкий signal_quality → высокая ML uncertainty |
| 10 | Cascade imminent | Cascade probability → высокая ML uncertainty |
| 11 | Correlation break | Корреляционный разрыв → высокая correlation uncertainty |
| 12 | High transition | Нестабильный режим → высокая transition uncertainty |
| 13 | EMA smoothing | persistent_score сглаживается через EMA |
| 14 | Spike detection | Резкое ухудшение → spike_score > 0 |
| 15 | Execution modes | Правильные рекомендации: Normal/Conservative/DefensiveOnly/Halt |
| 16 | Top-3 drivers | Ранжирование по вкладу в aggregate_score |
| 17 | Cooldown activation | 3+ Extreme подряд → cooldown active |
| 18 | Diagnostics tracking | Счётчики assessments, vetoes, cooldowns |
| 19 | Feedback buffer | record_feedback() сохраняет данные |
| 20 | reset_state() | Очищает всё внутреннее состояние |
| 21 | Boundary guarantees | size_multiplier ∈ [size_floor, 1.0], threshold ∈ [1.0, ceiling] |
| 22 | Monotonicity | Ухудшение условий → рост score |
| 23 | Custom weights | Нестандартные веса влияют на результат |
| 24 | Stale feed | Несвежий feed → высокая operational uncertainty |
| 25 | model_version | Всегда = 1 (текущая версия) |

---

## Планы развития

### Champion-Challenger модели

- Параллельный запуск альтернативных моделей неопределённости
- Сравнение калибровки и P&L impact через champion_challenger модуль
- Автоматическое продвижение лучшей модели

### Cross-asset uncertainty

- Агрегация неопределённости по всем торгуемым парам
- Системный скор: «рынок в целом непредсказуем»
- Глобальный killswitch при системной неопределённости

### PostgreSQL persistence для калибровки

- Сохранение feedback buffer в PostgreSQL
- Историческая аналитика calibration_error
- Тренды по измерениям: какие источники неопределённости растут

### Replay-based weight optimization

- Оптимизация весов 9 измерений через replay backtest
- Целевая функция: минимизация calibration_error + максимизация risk-adjusted return
- Cross-validation по временным окнам для предотвращения overfitting
