# Подробный разбор модуля world_model

Временный аналитический документ.

Источник разбора: текущая реализация `src/world_model`, её реальные потребители (`pipeline`, `strategy_allocator`, `decision`, `uncertainty`), конфигурация и unit-тесты на момент 2026-04-07.

Документ описывает не только публичный API, но и фактическое runtime-поведение модуля в основном контуре USDT-M futures бота.

**Статус после рефакторинга 2026-04-07:** все ниже найденные проблемы исправлены. 401/401 тестов проходят.

## 1. Роль модуля в системе

`world_model` отвечает не за генерацию сигналов и не за вычисление индикаторов, а за **семантическую интерпретацию уже готового `FeatureSnapshot`** в дискретное состояние рынка.

Практически это слой ответа на вопрос:

- что за рынок перед нами прямо сейчас;
- насколько это состояние хрупкое;
- насколько оно устойчиво;
- какие стратегии в таком состоянии уместны;
- есть ли признаки приближающегося перехода в другое состояние.

Фактическое место в pipeline:

```text
MarketDataGateway
  -> FeatureEngine / AdvancedFeatureEngine
  -> FeatureSnapshot
  -> WorldModel::update(snapshot)
  -> RegimeEngine::classify(snapshot)
  -> UncertaintyEngine::assess(snapshot, regime, world, ...)
  -> StrategyAllocator::allocate(..., world, uncertainty)
  -> DecisionAggregation
  -> Execution
```

То есть `world_model` стоит между сырыми признаками рынка и стратегическим оркестратором.

## 2. Состав модуля

В `src/world_model` находятся:

| Файл | Назначение |
|---|---|
| `world_model_engine.hpp` | интерфейс и основная реализация движка мировой модели |
| `world_model_engine.cpp` | вся логика классификации, хрупкости, вероятностей, suitability и feedback |
| `world_model_config.hpp` | пороги, hysteresis, history, suitability, feedback-конфиг |
| `world_model_types.hpp` | типы состояний, snapshot мировой модели, explainability, feedback |
| `world_model_history.hpp` | per-symbol контекст: история, dwell, матрица переходов |
| `world_model.md` | этот временный аналитический документ |

Сборка:

```cmake
add_library(tb_world_model STATIC world_model_engine.cpp)
target_link_libraries(tb_world_model PUBLIC tb_common tb_features tb_logging tb_clock)
```

Это важное наблюдение: модуль зависит только от `features`, `clock`, `logger`, `common` и **не зависит напрямую** от `regime`, `decision`, `uncertainty` или `strategy`. Он самодостаточен как state-classifier.

## 3. Публичный API

Основной интерфейс `IWorldModelEngine` даёт три группы возможностей:

1. Основной runtime API:
   - `update(const FeatureSnapshot&) -> WorldModelSnapshot`
   - `current_state(symbol) -> optional<WorldModelSnapshot>`

2. Feedback loop:
   - `record_feedback(const WorldStateFeedback&)`
   - `performance_stats(state, strategy)`

3. Governance:
   - `model_version()`

В production используется `RuleBasedWorldModelEngine`.

## 4. Что хранит модуль внутри

Движок stateful. Для каждого символа он ведёт `SymbolContext`, где хранится:

- последний подтверждённый `WorldModelSnapshot`;
- кандидат на переход (`candidate_state`, `candidate_ticks`);
- `dwell_ticks` — сколько тиков подряд живёт текущее состояние;
- история последних состояний и их fragility/confidence;
- матрица переходов `9x9`;
- счётчики эмпирической персистентности.

Также глобально движок хранит `feedback_stats_` по ключу `state:strategy_id`.

Следствие: это не stateless classifier, а **онлайн-модель состояния рынка с памятью**.

## 5. Какие состояния умеет определять модуль

Всего реализовано 9 состояний:

1. `StableTrendContinuation`
2. `FragileBreakout`
3. `CompressionBeforeExpansion`
4. `ChopNoise`
5. `ExhaustionSpike`
6. `LiquidityVacuum`
7. `ToxicMicrostructure`
8. `PostShockStabilization`
9. `Unknown`

Для совместимости с более грубыми потребителями они дополнительно маппятся в `WorldStateLabel`:

- `Stable`
- `Transitioning`
- `Disrupted`
- `Unknown`

## 6. Как работает `update()`

`update(snapshot)` — центральная функция модуля. По шагам она делает следующее:

1. Берёт текущий timestamp из `clock_`.
2. Достаёт или создаёт `SymbolContext` для символа.
3. Вызывает `classify_immediate()` — чистая классификация без hysteresis.
4. Вызывает `apply_hysteresis()` — сглаживание переходов.
5. Регистрирует переход в матрице переходов.
6. Считает:
   - `fragility`
   - `persistence`
   - `transition`
   - `confidence`
   - `state_probabilities`
   - `strategy_suitability`
7. Генерирует explainability:
   - `checked_conditions`
   - `triggered_conditions`
   - `top_drivers`
   - `summary`
8. Обновляет `dwell_ticks`.
9. Добавляет запись в историю.
10. Сохраняет результат как текущий snapshot символа.

Именно этот объект затем уходит в pipeline.

## 7. Immediate classification: порядок правил

Классификация в `classify_immediate()` жёстко приоритетная. Это очень важно: если сработало раннее правило, все последующие уже не проверяются как итоговое состояние.

Текущий порядок такой:

1. `InsufficientData -> Unknown`
2. `PostShockStabilization`
3. `ToxicMicrostructure`
4. `LiquidityVacuum_CriticalSpread`
5. `LiquidityVacuum_Skew`
6. `ExhaustionSpike`
7. `FragileBreakout`
8. `CompressionBeforeExpansion`
9. `StableTrendContinuation`
10. `ChopNoise`
11. `NoRuleTriggered -> Unknown`

### Почему порядок важен

Например, если рынок одновременно выглядит как тренд и как токсичная микроструктура, итогом будет `ToxicMicrostructure`, потому что это правило стоит выше `StableTrendContinuation`.

То есть логика модуля не voting-based, а **priority-based**.

## 8. Какой набор индикаторов реально использует world_model

Модуль сам **ничего не вычисляет**, а читает готовые поля из `FeatureSnapshot`.

### 8.1. Используемые технические индикаторы

| Индикатор | Источник | Где используется |
|---|---|---|
| `sma_valid` | `features` | только как часть indicator coverage / минимального набора данных |
| `rsi_14` | `features` | `ExhaustionSpike`, `StableTrendContinuation`, `ChopNoise`, top drivers |
| `bb_percent_b` | `features` | `FragileBreakout` |
| `bb_bandwidth` | `features` | `CompressionBeforeExpansion` |
| `atr_14_normalized` | `features` | `CompressionBeforeExpansion` |
| `adx` | `features` | `StableTrendContinuation`, `ChopNoise`, top drivers |
| `volatility_5/20` | `features` | `PostShockStabilization`, `FragileBreakout`, `CompressionBeforeExpansion`, fragility |
| `momentum_5` | `features` | `ExhaustionSpike`, top drivers |

### 8.2. Используемые микроструктурные признаки

| Признак | Источник | Где используется |
|---|---|---|
| `spread_bps` | `features` | `ToxicMicrostructure`, `LiquidityVacuum`, `ChopNoise`, fragility, execution suitability |
| `book_instability` | `features` | `ToxicMicrostructure`, fragility, top drivers |
| `aggressive_flow` | `features` | `ToxicMicrostructure`, top drivers |
| `book_imbalance_5` | `features` | `FragileBreakout` |
| `liquidity_ratio` | `features` | `LiquidityVacuum`, fragility, top drivers |
| `vpin` | `advanced_features` | indicator coverage, fragility, top drivers, data quality |

### 8.3. Используемые execution/context поля

| Поле | Где используется |
|---|---|
| `execution_context.is_feed_fresh` | штраф в `assess_data_quality()` |
| `execution_context.estimated_slippage_bps` | execution suitability для стратегий |

## 9. Что world_model НЕ использует, хотя признаки уже есть в проекте

В `FeatureSnapshot` уже доступны и вычисляются upstream следующие признаки, но `world_model` их сейчас не использует:

- `ema_20`, `ema_50`
- `macd_line`, `macd_signal`, `macd_histogram`
- `obv`, `obv_normalized`
- `trade_vwap`
- `buy_sell_ratio`
- `weighted_mid_price`
- `vp_poc`, `vp_value_area_high`, `vp_value_area_low`, `vp_price_vs_poc`
- `cusum_positive`, `cusum_negative`, `cusum_regime_change`
- `tod_volatility_mult`, `tod_volume_mult`, `tod_alpha_score`

Это не означает, что модуль сломан. Это означает, что текущая state-model использует **ядро признаков**, а не весь доступный feature-space.

## 10. Достаточны ли реализованные индикаторы для проекта

Ответ надо разделять на два уровня.

### 10.1. Для текущей логики самого world_model

Да. Все индикаторы, которые `world_model` реально читает в коде, в проекте реализованы и заполняются upstream:

- ADX
- RSI
- Bollinger `%B` и bandwidth
- ATR normalized
- volatility 5/20
- momentum 5
- spread_bps
- book imbalance
- liquidity_ratio
- book_instability
- aggressive_flow
- VPIN

Для **текущего набора правил классификации** покрытие полное.

### 10.2. Для более широкого стратегического словаря проекта

Не полностью.

Причина: таблица `SuitabilityConfig` уже оперирует такими стратегическими ярлыками, как:

- `ema_pullback`
- `vwap_reversion`
- `volume_profile`
- `rsi_divergence`
- `momentum`
- `breakout`
- `mean_reversion`
- `vol_expansion`

Но сама мировая модель **не использует соответствующие профильные признаки** для части этих направлений:

- для `ema_pullback` не используются `ema_20/ema_50`;
- для `vwap_reversion` не используется VWAP или distance-to-VWAP;
- для `volume_profile` не используются `vp_poc` и `value area`;
- для смены режима не используется `cusum_regime_change`;
- для внутридневной сезонности не используется Time-of-Day профиль.

Следствие: модуль умеет **эвристически назначать suitability** таким стратегиям, но делает это не на их родных индикаторах, а на более общей market-state картине.

Иными словами:

- для текущей state machine coverage достаточен;
- для амбиции быть полноценной стратегически-семантической моделью — coverage пока неполный.

## 11. Важное отличие между “заявлено в таблице” и “реально активно в pipeline”

По текущему `TradingPipeline` реально регистрируется только одна стратегия:

- `scalp_engine`

`world_model` же содержит suitability-таблицу для девяти strategy id, включая `momentum`, `breakout`, `ema_pullback`, `volume_profile` и т.д.

Это означает, что сегодня есть два слоя:

1. **Фактический production path** — в нём реально важен в основном `scalp_engine`.
2. **Задекларированная архитектура будущего/расширенного набора стратегий** — под неё suitability уже подготовлен.

Следовательно, вопрос “все ли нужные индикаторы реализованы” зависит от того, о каком слое идёт речь:

- для текущего production path — почти да;
- для всей задекларированной стратегической матрицы — нет, не полностью.

## 12. Как работает hysteresis

`apply_hysteresis()` нужен, чтобы не дёргаться на шуме.

Логика такая:

- первый вызов принимает immediate state сразу;
- опасные состояния (`LiquidityVacuum`, `ToxicMicrostructure`, `ExhaustionSpike`) проходят без задержки;
- `PostShockStabilization` тоже может пройти сразу после шоковых состояний;
- все остальные переходы требуют:
  - `candidate_ticks >= confirmation_ticks`
  - `dwell_ticks >= min_dwell_ticks`

Если переход заблокирован, в explanation выставляется:

- `hysteresis_overrode = true`
- `confirmation_ticks_remaining`
- `dwell_ticks`

### Важное наблюдение

В конфиге есть `world_model.hysteresis.min_confidence_to_switch`, но в коде world_model этот параметр **не используется вообще**.

То есть hysteresis сейчас зависит только от тиков подтверждения и dwell-time, но не от confidence.

## 13. Fragility, persistence, transition, confidence

### 13.1. Fragility

`compute_fragility()` строит composite score из:

- базовой fragility состояния;
- spread stress;
- book instability;
- volatility acceleration;
- liquidity imbalance;
- recent transition instability;
- VPIN toxicity.

Это не просто “ярлык состояния”, а отдельная численная оценка качества рыночной среды.

### 13.2. Persistence

`compute_persistence()` смешивает:

- базовую persistence для состояния;
- эмпирическую persistence из фактической истории переходов.

То есть модель со временем адаптируется к наблюдаемой динамике символа.

### 13.3. TransitionContext

`compute_transition()` считает:

- `tendency`
- `velocity`
- `pressure`
- `transitions_recent`

`tendency` основана не на статистической модели, а на ручном ранге “качества состояния” (`state_quality`).

### 13.4. Confidence

`compute_confidence()` смешивает:

- качество данных;
- coverage индикаторов;
- dwell stability;
- отсутствие конфликта с hysteresis;
- proximity сработавших правил.

Для `Unknown` confidence дополнительно режется в 3 раза.

## 14. Вероятности по состояниям

`compute_state_probabilities()` работает так:

1. Берёт `confidence` основного состояния.
2. Остаток массы равномерно распределяет по всем 9 состояниям.
3. Если накоплено достаточно переходов (`total_transitions > 20`), подмешивает 20% эмпирического prior из матрицы переходов.
4. Нормализует сумму до 1.

Это не полноценная HMM и не Bayesian filter. Это pragmatic probability wrapper над rule-based primary state.

## 15. Strategy suitability

`compute_suitability()` формирует для каждой стратегии:

- `signal_suitability`
- `execution_suitability`
- `risk_suitability`
- `suitability = signal * execution * risk`
- `vetoed`

Также применяется feedback-blend через `ema_win_rate`, если накоплено достаточно сделок.

### Практически важно

В опасных состояниях:

- `LiquidityVacuum`
- `ToxicMicrostructure`
- `ExhaustionSpike`

стратегия `scalp_engine` жёстко veto'ится прямо в world_model.

Это особенно важно для текущего production path, потому что именно `scalp_engine` сейчас реально зарегистрирован в pipeline.

## 16. Feedback loop

`record_feedback()` собирает по `(state, strategy)`:

- total trades
- wins
- total pnl
- EMA win rate
- EMA expectancy
- average slippage
- max adverse excursion

Это даёт модулю возможность постепенно подправлять suitability в зависимости от фактических результатов торговли.

### Но есть ограничение

В конфиге есть `feedback.enabled`, `min_samples_per_state`, `max_threshold_drift_pct`, `evaluation_window`, но в текущей реализации:

- `feedback.enabled` не проверяется;
- threshold drift не применяется;
- пороги правил автоматически не обучаются;
- evaluation window не используется.

То есть feedback сейчас влияет только на suitability, но **не адаптирует сами правила классификации**.

## 17. Реальная интеграция в pipeline

В `TradingPipeline` мир обновляется сразу после готового `FeatureSnapshot`:

```text
world = world_model_->update(snapshot)
regime = regime_engine_->classify(snapshot)
uncertainty = uncertainty_engine_->assess(snapshot, regime, world, ...)
allocation = strategy_allocator_->allocate(..., regime, world, uncertainty)
decision = decision_engine_->aggregate(..., world, uncertainty, ...)
```

Реальные последствия этого:

1. `world_model` напрямую участвует в strategy allocation.
2. `world_model` влияет на uncertainty engine.
3. `world_model` влияет на decision record через `world.label`.
4. В pipeline есть **жёсткий блок** на `WorldState::ChopNoise` для новых входов.

То есть модуль не декоративный: он реально режет торговлю и переназначает веса стратегий.

## 18. Что именно блокирует торговлю сейчас

Явная жёсткая блокировка в pipeline сделана для:

- `world.state == ChopNoise`

Для остальных опасных состояний явного `return` в pipeline нет, но они всё равно влияют через:

- suitability veto на `scalp_engine`;
- allocator;
- uncertainty / execution cost / downstream layers.

При текущем составе активных стратегий это на практике тоже часто означает отсутствие входа.

## 19. Тестовое покрытие

Есть отдельный `tests/unit/world_model/world_model_test.cpp`.

Покрыты:

- все 9 состояний;
- to_label mapping;
- current_state cache;
- strategy suitability;
- veto в опасных состояниях;
- конфигурируемые пороги;
- explanation/top_drivers;
- hysteresis;
- dwell/persistence;
- state probabilities;
- feedback;
- versioning;
- config validation;
- multi-symbol isolation;
- transition tendency;
- composite fragility.

Это хорошее покрытие для rule-based state module.

### Что не покрыто напрямую

- ветка `transition prior` в `compute_state_probabilities()` при богатой матрице переходов;
- неиспользуемые конфиги, потому что они фактически не участвуют в коде;
- штраф confidence через `hysteresis.min_confidence_to_switch`, потому что такого поведения нет;
- regime adaptation, потому что она не реализована;
- cross-module влияние world suitability на allocator при множестве реально зарегистрированных стратегий.

## 20. Исправления (рефакторинг 2026-04-07)

Все ниже перечисленные проблемы **исправлены**. 401/401 тестов проходят.

1. `min_valid_indicators` поднят с 2 до 4
2. `top_drivers` нормализованы в [0,1]
3. Удалены мёртвые конфиги: `RegimeAdaptationConfig`, `min_confidence_to_switch`, `transition_matrix_window`, `alpha_decay_weight`, `FeedbackConfig` лишние поля
4. `int total = 7; total = 12;` → `constexpr int total = 12;`
5. `feedback.enabled` проверяется в `record_feedback()` и `compute_suitability()`
6. `PostShockStabilization` срабатывает после `ToxicMicrostructure`
7. `ExhaustionSpike` proximity исправлен
8. Execution suitability — USDT-M futures пороги (10 bps вместо spot 50 bps)
9. Risk suitability дифференцирован: LiqVacuum=0.15, Toxic=0.20, Exhaustion=0.25
10. `assess_data_quality` — 12 индикаторов (было 8)
11. Научные обоснования: Wilder 1978, Bollinger 2001, Easley/O'Hara 2012, Cont et al 2014

SMA здесь выступает скорее как индикатор “техконтекст вообще есть”, а не как полноценный вход модели.

## 21. Итог по индикаторам

### Реализовано и реально используется world_model

Да, всё необходимое для **текущих правил** реализовано.

### Реализовано в проекте, но не используется world_model

Да, таких признаков много:

- EMA
- MACD
- OBV
- VWAP
- Volume Profile
- CUSUM
- Time-of-Day
- часть microstructure-полей

### Следовательно

Правильный итоговый вывод такой:

1. Для текущей rule-based мировой модели **все необходимые индикаторы реализованы**.
2. Для более амбициозной мировой модели, которая должна полноценно покрывать весь задекларированный набор стратегий проекта, **coverage пока неполный**.
3. Самый заметный стратегический разрыв: модель оценивает пригодность `ema_pullback`, `vwap_reversion`, `volume_profile`, но не использует EMA/VWAP/Volume Profile как первичные драйверы состояния.

## 22. Краткий вывод одной фразой

Текущий `world_model` — это не статистическая вероятностная модель, а качественно сделанный stateful rule-based market-state classifier с history, hysteresis, fragility, suitability и feedback, который хорошо покрывает текущий production-path, но ещё не использует весь набор сильных индикаторов, уже доступных в `features`, для полной стратегической семантики проекта.