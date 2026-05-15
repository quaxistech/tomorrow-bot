# Подробный разбор модуля uncertainty

Временный аналитический документ.

Источник разбора: текущая реализация `src/uncertainty`, реальные потребители (`pipeline`, `risk`, `decision`, `strategy_allocator`, `execution_alpha`, `execution`), unit-тесты и upstream-источники данных на момент 2026-04-08.

Документ описывает не только публичный API, но и фактическое runtime-поведение модуля в основном контуре USDT-M futures scalp-бота.

## 1. Роль модуля в системе

`uncertainty` отвечает за **оценку общей неопределённости торгового контекста**.

Если:

- `world_model` отвечает на вопрос «какое сейчас состояние рынка»;
- `regime` отвечает на вопрос «какой торговый режим сейчас доминирует»;

то `uncertainty` отвечает на практический вопрос:

- насколько вообще можно доверять текущему сетапу;
- нужно ли уменьшать размер позиции;
- нужно ли повышать conviction threshold;
- нужно ли перевести исполнение в defensive mode;
- нужно ли на время запретить новые входы.

Фактическое место в pipeline:

```text
MarketDataGateway
  -> FeatureEngine / AdvancedFeatureEngine
  -> FeatureSnapshot
  -> WorldModel::update(snapshot)
  -> RegimeEngine::classify(snapshot)
  -> PortfolioEngine::snapshot()
  -> ML snapshot assembly (partial)
  -> UncertaintyEngine::assess(snapshot, regime, world, portfolio, ml)
  -> StrategyAllocator::allocate(..., uncertainty)
  -> DecisionAggregation(..., uncertainty)
  -> RiskEngine(..., uncertainty)
  -> ExecutionAlpha(..., uncertainty)
  -> Execution
```

То есть это не декоративный score. `uncertainty` реально влияет на:

- hard block новых входов;
- размер позиции;
- пороги принятия решения;
- режим исполнения;
- risk throttling;
- allocator weights.

## 2. Состав модуля

В `src/uncertainty` находятся:

| Файл | Назначение |
|---|---|
| `uncertainty_engine.hpp` | интерфейс `IUncertaintyEngine` и реализация `RuleBasedUncertaintyEngine` |
| `uncertainty_engine.cpp` | вся логика расчёта 9 размерностей неопределённости, stateful hysteresis, cooldown, top drivers |
| `uncertainty_types.hpp` | config, enums, snapshot, diagnostics, feedback, drivers |
| `uncertainty.md` | этот временный аналитический документ |

Сборка:

```cmake
add_library(tb_uncertainty STATIC uncertainty_engine.cpp)
target_link_libraries(tb_uncertainty PUBLIC tb_common tb_features tb_regime tb_world_model tb_logging tb_clock tb_portfolio tb_ml)
```

Важные наблюдения:

- модуль напрямую зависит от `features`, `regime`, `world_model`, `portfolio`, `ml`, `logging`, `clock`;
- зависимости на `metrics` нет вообще;
- отдельной конфигурации в `AppConfig` / YAML **нет**;
- в `TradingPipeline` движок создаётся как `RuleBasedUncertaintyEngine(UncertaintyConfig{}, logger_, clock_)`, то есть всегда на compile-time defaults.

## 3. Публичный API

Интерфейс `IUncertaintyEngine` содержит:

1. `assess(features, regime, world)`
2. `assess(features, regime, world, portfolio, ml_signals)`
3. `current(symbol)`
4. `diagnostics()`
5. `record_feedback(feedback)`
6. `reset_state()`

В production path реально используется **v2 overload** с пятью аргументами.

Важный факт:

- `current()` используется только в unit-тестах;
- `diagnostics()` используется только в unit-тестах;
- `record_feedback()` внешними runtime-потребителями не вызывается вообще.

То есть в продакшн-контуре реально активен только `assess(...)`.

## 4. v1 и v2: как устроена совместимость

Модуль имеет две перегрузки `assess()`:

### 4.1. v1

```cpp
assess(features, regime, world)
```

Эта версия создаёт:

- пустой `PortfolioSnapshot`;
- нейтральный `MlSignalSnapshot` с `overall_health = Healthy`;

и делегирует в полную версию.

### 4.2. v2

```cpp
assess(features, regime, world, portfolio, ml_signals)
```

Это основной production path.

Следствие:

- старая v1-сигнатура сохранена ради обратной совместимости;
- реальная модель неопределённости уже давно опирается не только на индикаторы, но и на `portfolio` + `ML`.

## 5. Что модуль хранит внутри

`RuleBasedUncertaintyEngine` stateful. На каждый символ поддерживаются:

1. `snapshots_`
   - последний `UncertaintySnapshot`

2. `states_`
   - `ema_score`
   - `peak_score`
   - `prev_level`
   - `last_assess_ns`
   - `last_level_change_ns`
   - `cooldown_until_ns`
   - `consecutive_extreme`
   - `consecutive_high`
   - `shock_memory`

3. `diagnostics_`
   - глобальные счётчики и агрегаты

4. `feedback_buffer_`
   - буфер обратной связи для предполагаемой калибровки

Следствие: это не stateless score calculator, а **онлайн stateful uncertainty layer**.

## 6. Полный алгоритм `assess()`

Полная версия `assess()` работает так:

1. читает `now = clock_->now()`;
2. считает 9 размерностей неопределённости;
3. агрегирует их в общий `aggregate_score`;
4. при `world.fragility.value > 0.7` добавляет global stress amplifier `+0.1`;
5. читает прошлое состояние символа;
6. считает `persistent_score` через EMA;
7. считает `spike_score = max(0, raw - persistent)`;
8. через hysteresis определяет итоговый `UncertaintyLevel`;
9. маппит уровень в `recommended_action`;
10. считает `size_multiplier = max(size_floor, 1 - aggregate_score)`;
11. считает `threshold_multiplier = min(threshold_ceiling, 1 + aggregate_score)`;
12. выбирает `execution_mode`;
13. рассчитывает `cooldown`;
14. выбирает top-3 drivers;
15. строит explanation string;
16. обновляет локальное состояние символа;
17. обновляет diagnostics;
18. кэширует snapshot;
19. логирует итог;
20. возвращает `UncertaintySnapshot`.

## 7. Девять размерностей неопределённости

Модель использует 9 независимых компонент.

### 7.1. `regime_uncertainty`

Формула:

$$
regime\_uncertainty = clamp(1 - regime.confidence, 0, 1)
$$

Это самая простая размерность: чем ниже confidence классификатора режима, тем выше uncertainty.

Используемые входы:

- `regime.confidence`

### 7.2. `signal_uncertainty`

Базовое значение:

$$
base = 0.3
$$

Дальше применяются эвристики:

- конфликт RSI и MACD;
- конфликт EMA-тренда и экстремального RSI;
- штраф за малое число валидных индикаторов.

Конкретно:

1. если `RSI>70` и `MACD_histogram>0`, uncertainty уменьшается на `0.15`;
2. если `RSI>70` и `MACD_histogram<0`, uncertainty увеличивается на `0.2`;
3. если `EMA20>EMA50` и RSI уже перекуплен, uncertainty увеличивается на `0.1`;
4. если валидных среди `{RSI, MACD, ADX, BB, EMA}` меньше 3, uncertainty увеличивается на `0.2`.

Используемые входы:

- `rsi_14`, `rsi_valid`
- `macd_histogram`, `macd_valid`
- `ema_20`, `ema_50`, `ema_valid`
- `adx_valid`
- `bb_valid`

### 7.3. `data_quality_uncertainty`

Составляется из простых штрафов:

- `book_quality != Valid` -> `+0.3`
- `!is_feed_fresh` -> `+0.3`
- `spread_bps > 30` -> `+0.2`
- `!sma_valid` -> `+0.1`
- `!spread_valid` -> `+0.1`

Используемые входы:

- `book_quality`
- `execution_context.is_feed_fresh`
- `microstructure.spread_bps`, `spread_valid`
- `technical.sma_valid`

### 7.4. `execution_uncertainty`

Состав:

- вклад спреда: `min(0.4, spread_bps / 100)`
- вклад проскальзывания: `min(0.3, estimated_slippage_bps / 50)`
- дополнительный штраф `+0.2` при `liquidity_ratio > 3.0`

Используемые входы:

- `microstructure.spread_bps`, `spread_valid`
- `execution_context.estimated_slippage_bps`, `slippage_valid`
- `microstructure.liquidity_ratio`, `liquidity_valid`

Это место содержит важную логическую проблему, описанную ниже: `liquidity_ratio > 3.0` недостижим при реальном upstream.

### 7.5. `portfolio_uncertainty`

Если портфель пустой, возвращается `0.0`.

Иначе:

- позиция > 60% капитала -> `+0.5`
- позиция > 40% капитала -> `+0.3`
- utilization > 90% -> `+0.4`
- utilization > 70% -> `+0.2`
- дневной PnL < -2% капитала -> `+0.4`
- дневной PnL < -1% капитала -> `+0.2`
- больше 4 позиций -> `+0.1`

Используемые входы:

- `portfolio.positions[].notional`
- `portfolio.total_capital`
- `portfolio.capital_utilization_pct`
- `portfolio.pnl.realized_pnl_today`
- `portfolio.positions.size()`

### 7.6. `ml_uncertainty`

Логика:

1. если `overall_health ∈ {Failed, Stale}` -> сразу `0.7`;
2. иначе `base = 1 - signal_quality`;
3. `cascade_imminent` -> `+0.3`, иначе `+0.3 * cascade_probability`;
4. `recommended_wait_periods < 0` -> `+0.2`.

Используемые входы:

- `overall_health`
- `signal_quality`
- `cascade_imminent`
- `cascade_probability`
- `recommended_wait_periods`

### 7.7. `correlation_uncertainty`

Логика:

- `correlation_break == true` -> `0.8`
- иначе `1 - correlation_risk_multiplier`

Используемые входы:

- `correlation_break`
- `correlation_risk_multiplier`

### 7.8. `transition_uncertainty`

Логика:

- если есть `regime.last_transition`, то

$$
base = 0.3 + 0.4 \cdot (1 - transition.confidence)
$$

- если `regime.stability < 0.3`, добавляется ещё `0.2`.

Используемые входы:

- `regime.last_transition`
- `regime.last_transition->confidence`
- `regime.stability`

### 7.9. `operational_uncertainty`

Логика:

- если `!is_feed_fresh`, сразу `0.7`;
- иначе:
  - `book_quality != Valid` -> `+0.3`
  - `estimated_slippage_bps > 20` -> `+0.2`

Используемые входы:

- `execution_context.is_feed_fresh`
- `book_quality`
- `execution_context.estimated_slippage_bps`, `slippage_valid`

## 8. Агрегация размерностей

Используется взвешенное среднее по конфигу:

- `w_regime = 0.25`
- `w_signal = 0.20`
- `w_data_quality = 0.10`
- `w_execution = 0.10`
- `w_portfolio = 0.10`
- `w_ml = 0.10`
- `w_correlation = 0.05`
- `w_transition = 0.05`
- `w_operational = 0.05`

Сумма весов по умолчанию = `1.0`.

### 8.1. Regime-aware reweighting

Если `regime.label == Volatile`:

- `w_data *= 1.5`
- `w_execution *= 1.5`
- `w_signal *= 0.8`

Если `regime.label == Unclear`:

- все веса умножаются на `1.2`

После этого веса нормализуются.

### 8.2. Tail-stress amplifier

За каждую размерность `> 0.8` добавляется ещё `+0.05` к aggregate score.

Следствие:

- модель не просто усредняет, а усиливает tail-risk;
- несколько экстремальных компонент быстро толкают score к `High/Extreme`.

### 8.3. World fragility amplifier

После агрегации:

- если `world.fragility.valid && world.fragility.value > 0.7`, score увеличивается ещё на `0.1`.

То есть `world_model` влияет на uncertainty только через fragility, и только как post-aggregate amplifier.

## 9. Stateful слой: persistent score, hysteresis, cooldown

### 9.1. Persistent score

EMA:

$$
persistent = \alpha \cdot raw + (1-\alpha) \cdot prev\_ema
$$

где `alpha = 0.15`.

### 9.2. Spike score

$$
spike = max(0, raw - persistent)
$$

Это быстрый индикатор внезапного ухудшения условий.

### 9.3. Hysteresis по уровням

Пороговые зоны:

- `< 0.25` -> `Low`
- `[0.25, 0.50)` -> `Moderate`
- `[0.50, 0.75)` -> `High`
- `>= 0.75` -> `Extreme`

Но прямой переход между уровнями не делается сразу:

- для повышения уровня требуется `threshold + hysteresis_up`
- для понижения уровня требуется `threshold - hysteresis_down`

По умолчанию:

- `hysteresis_up = 0.03`
- `hysteresis_down = 0.05`

То есть понижение более инерционное, чем повышение.

### 9.4. Cooldown

Комментарий и код декларируют:

- после 3 подряд `Extreme` активируется кулдаун на 60 секунд.

Но фактический порядок вызовов такой:

1. `compute_cooldown(state, now)` вызывается **до** `update_state(...)`;
2. только потом `consecutive_extreme` увеличивается.

Следствие:

- кулдаун становится видимым не на 3-й экстремальной оценке, а только на **следующей**, то есть фактически на 4-й.

Тесты подтверждают именно это фактическое поведение.

## 10. Что возвращает модуль наружу

`UncertaintySnapshot` содержит:

- `level`
- `aggregate_score`
- `dimensions`
- `recommended_action`
- `size_multiplier`
- `threshold_multiplier`
- `top_drivers`
- `execution_mode`
- `cooldown`
- `persistent_score`
- `spike_score`
- `calibration_confidence`
- `model_version`
- `explanation`

### 10.1. Action mapping

- `Low` -> `Normal`
- `Moderate` -> `ReducedSize`
- `High` -> `HigherThreshold`
- `Extreme` -> `NoTrade`

### 10.2. Size multiplier

$$
size\_multiplier = max(size\_floor, 1 - aggregate\_score)
$$

При `size_floor = 0.10` размер никогда не падает ниже 10%, если только pipeline не среагирует на `NoTrade` отдельно.

### 10.3. Threshold multiplier

$$
threshold\_multiplier = min(threshold\_ceiling, 1 + aggregate\_score)
$$

При дефолтах максимум = `2.0`, потому что `aggregate_score ∈ [0,1]`, хотя `threshold_ceiling` задан как `3.0`.

## 11. Реальная интеграция в pipeline и downstream

`uncertainty` реально влияет на систему в нескольких местах.

### 11.1. TradingPipeline

После `world_model` и `regime` pipeline вызывает:

```text
portfolio_snap = portfolio_->snapshot()
uncertainty = uncertainty_engine_->assess(snapshot, regime, world, portfolio_snap, ml_snapshot_)
```

И дальше есть hard block:

- если `recommended_action == NoTrade`, тик останавливается полностью.

### 11.2. StrategyAllocator

Все веса стратегий дополнительно умножаются на:

- `uncertainty.size_multiplier`

### 11.3. Decision engine

`uncertainty` влияет на:

- global veto (`NoTrade`);
- `effective_threshold = base_threshold * uncertainty.threshold_multiplier`.

### 11.4. Risk

`risk` использует:

- `uncertainty.size_multiplier` для лимитов;
- `uncertainty.cooldown.active` для throttle;
- `uncertainty.execution_mode == HaltNewEntries` для deny новых входов.

### 11.5. Execution Alpha

`execution_alpha` использует:

- `execution_mode == HaltNewEntries` -> полный блок;
- `execution_mode == DefensiveOnly` -> блок новых входов;
- `level == Extreme` -> блок исполнения;
- `urgency_score *= size_multiplier`.

Вывод: `uncertainty` встроен глубоко и влияет на весь downstream stack.

## 12. Что реально активно в production path

В `TradingPipeline` uncertainty вызывается **до** части поздних ML-обновлений.

До вызова `uncertainty_engine_->assess(...)` в `ml_snapshot_` уже обновлены:

- entropy filter;
- liquidation cascade;
- correlation monitor.

После вызова uncertainty pipeline ещё обновляет:

- fingerprint edge;
- bayesian adapted thresholds;
- Thompson `recommended_wait_periods`;
- `ml_snapshot_.compute_aggregates()`.

Следствие:

- uncertainty в текущем тике видит не весь ML-контекст;
- `recommended_wait_periods`, `overall_health`, `should_block_trading`, `combined_risk_multiplier` для текущего тика в uncertainty не участвуют полноценно;
- часть ML-входов для uncertainty является либо stale, либо дефолтной.

## 13. Тестовое покрытие

Есть отдельный `tests/unit/uncertainty/uncertainty_test.cpp`.

Покрыты:

- низкая и высокая неопределённость;
- `size_multiplier` / `threshold_multiplier`;
- extreme -> `NoTrade`;
- v2 `portfolio` + `ML` path;
- portfolio concentration;
- ML degradation;
- correlation break;
- transition uncertainty;
- EMA smoothing;
- spike detection;
- execution mode;
- top drivers ranking;
- cooldown activation;
- diagnostics;
- feedback recording;
- `reset_state()`;
- bounds guarantees;
- monotonicity;
- custom weights.

### Что тесты не покрывают напрямую

- отсутствие YAML/AppConfig интеграции;
- несоответствие `liquidity_ratio > 3.0` реальному upstream-диапазону;
- лаг на один тик у `DefensiveOnly`;
- лаг на один тик у cooldown activation;
- фактическое отсутствие runtime-consumer для `record_feedback()`;
- незаполняемые поля `avg_assessment_latency_us`, `max_assessment_latency_us`, `calibration_error`;
- timing problem: uncertainty вызывается раньше финальной сборки `ml_snapshot_`.

## 14. Какие входы реально использует uncertainty

`uncertainty` использует не только индикаторы, но и контекст из четырёх подсистем.

### 14.1. Технические и микроструктурные признаки

Используются:

- `sma_valid`
- `rsi_14`, `rsi_valid`
- `macd_histogram`, `macd_valid`
- `ema_20`, `ema_50`, `ema_valid`
- `adx_valid`
- `bb_valid`
- `spread_bps`, `spread_valid`
- `liquidity_ratio`, `liquidity_valid`
- `estimated_slippage_bps`, `slippage_valid`
- `is_feed_fresh`
- `book_quality`

### 14.2. Regime inputs

Используются:

- `regime.confidence`
- `regime.label`
- `regime.stability`
- `regime.last_transition`

### 14.3. World model inputs

Используются:

- только `world.fragility.valid`
- только `world.fragility.value`

### 14.4. Portfolio inputs

Используются:

- `positions[].notional`
- `positions.size()`
- `total_capital`
- `capital_utilization_pct`
- `pnl.realized_pnl_today`

### 14.5. ML inputs

Используются:

- `overall_health`
- `signal_quality`
- `cascade_probability`
- `cascade_imminent`
- `correlation_break`
- `correlation_risk_multiplier`
- `recommended_wait_periods`

## 15. Какие данные есть в проекте, но uncertainty их не использует

### 15.1. Из `FeatureSnapshot`

Не используются, хотя доступны:

- `atr_14`, `atr_14_normalized`
- `obv`, `obv_normalized`
- `volatility_5`, `volatility_20`
- `momentum_5`, `momentum_20`
- `bb_percent_b`
- `plus_di`, `minus_di`
- `cusum_*`
- `vp_*`
- `tod_*`
- `buy_sell_ratio`
- `aggressive_flow`
- `trade_vwap`
- `book_imbalance_5`, `book_imbalance_10`
- `weighted_mid_price`
- `immediate_liquidity`
- `spread_cost_bps`
- `is_market_open`
- `book_instability`
- `vpin`, `vpin_toxic`

### 15.2. Из `MlSignalSnapshot`

Не используются, хотя присутствуют:

- `fingerprint_edge`
- `fingerprint_win_rate`
- `fingerprint_sample_count`
- `adapted_conviction_threshold`
- `adapted_atr_stop_mult`
- `entry_confidence`
- `combined_risk_multiplier`
- `should_block_trading`
- `block_reason`
- per-component statuses, кроме `overall_health`

### 15.3. Из `PortfolioSnapshot`

Не используются, хотя доступны:

- `exposure`
- `available_capital`
- `cash`
- `pending_orders`
- `pending_buy_count`, `pending_sell_count`
- `total_fees_today`
- `pnl.current_drawdown_pct`
- `pnl.consecutive_losses`

Следствие: текущий uncertainty layer использует только часть доступного context-space.

## 16. Все ли необходимые индикаторы и входы реализованы

Ответ нужно разделять на два уровня.

### 16.1. Для текущей логики самого uncertainty

Да.

Все поля, которые модуль реально читает из:

- `FeatureSnapshot`
- `RegimeSnapshot`
- `WorldModelSnapshot`
- `PortfolioSnapshot`
- `MlSignalSnapshot`

в проекте реализованы upstream.

Подтверждено по коду:

- `FeatureEngine` заполняет `spread_bps`, `liquidity_ratio`, `estimated_slippage_bps`, `is_feed_fresh`;
- `PortfolioEngine::snapshot()` заполняет `positions`, `total_capital`, `capital_utilization_pct`, `pnl.realized_pnl_today`;
- pipeline реально обновляет `signal_quality`, `cascade_probability`, `correlation_break`, `correlation_risk_multiplier`, `recommended_wait_periods` в `ml_snapshot_`.

### 16.2. Для более полной uncertainty-семантики проекта

Не полностью.

Причины:

1. uncertainty вообще не использует `ATR`, `OBV`, `VPIN`, `book_instability`, `CUSUM`, `ToD`, хотя для futures scalp-бота это полезные uncertainty-drivers;
2. uncertainty почти не использует `world_model`, кроме одного fragility amplifier;
3. uncertainty видит только часть ML-снимка на момент вызова;
4. uncertainty не имеет production-config из YAML, поэтому веса и thresholds не tunable в runtime.

Итог:

1. для текущей rule-based модели входное покрытие есть;
2. для полной production-семантики проекта покрытие и интеграция пока неполные.

## 17. Ключевые ограничения и ошибки логики

Ниже самые важные находки.

### 17.1. Нет YAML / AppConfig интеграции вообще

`UncertaintyConfig` не входит в `AppConfig`, не парсится из `config_loader`, не валидируется `ConfigValidator`.

Следствие:

- production не может настраивать веса и thresholds uncertainty;
- пороги и веса фиксированы в коде;
- научно обоснованная настройка невозможна без перекомпиляции.

### 17.2. `liquidity_ratio > 3.0` в execution uncertainty недостижим

Upstream `FeatureEngine` считает:

$$
liquidity\_ratio = \frac{\min(bid, ask)}{0.5 \cdot (bid + ask)}
$$

Следовательно, `liquidity_ratio ∈ [0,1]`.

Но `compute_execution_uncertainty()` проверяет:

```cpp
if (features.microstructure.liquidity_valid &&
    features.microstructure.liquidity_ratio > 3.0) {
    uncertainty += 0.2;
}
```

Это условие невозможно при реальных данных.

Следствие:

- часть execution uncertainty никогда не срабатывает;
- unit-тесты маскируют проблему, подставляя `liquidity_ratio = 5.0`, чего upstream не создаёт.

### 17.3. Cooldown активируется на тик позже, чем заявлено

Комментарий говорит про активацию после `3+ consecutive extreme`, но из-за порядка:

- сначала `compute_cooldown(state, now)`;
- потом `update_state(...)`;

кулдаун становится активен только на следующем вызове.

То есть фактическое поведение = 4-й extreme tick.

### 17.4. `DefensiveOnly` для high-uncertainty тоже с лагом

`determine_execution_mode()` проверяет:

```cpp
if (level == High && state.consecutive_high >= 3)
```

Но `state` здесь ещё старый, до `update_state(...)`.

Следствие:

- defensive escalation запаздывает на один тик.

### 17.5. ML snapshot для uncertainty неполон в момент вызова

`uncertainty_engine_->assess(...)` вызывается до того, как pipeline завершает:

- Thompson wait periods;
- compute_aggregates();
- overall health aggregation;
- часть поздних ML-полей.

Следствие:

- `recommended_wait_periods` может быть stale;
- `overall_health` может быть stale/default;
- `combined_risk_multiplier`, `should_block_trading`, `block_reason` вообще не участвуют.

### 17.6. `record_feedback()` архитектурно есть, но runtime-калибровки нет

Фактическое поведение `record_feedback()`:

- просто кладёт `feedback` в `feedback_buffer_`;
- увеличивает `feedback_samples`;
- пишет `last_feedback`.

Дальше эти данные нигде не используются.

Следствие:

- реальной online calibration нет;
- `feedback_buffer_` — пока мёртвый буфер.

### 17.7. `calibration_confidence` не является реальной калибровкой

`calibration_confidence` считается только от количества samples:

- меньше `min_feedback_samples` -> `0.5`
- дальше плавно растёт и cap = `0.95`

При этом:

- prediction error не анализируется;
- Brier score не считается;
- calibration drift не отслеживается.

То есть это не calibration confidence, а sample-count confidence proxy.

### 17.8. Diagnostics частично мёртвые

Есть поля:

- `avg_assessment_latency_us`
- `max_assessment_latency_us`
- `calibration_error`

Но в коде они нигде не обновляются.

Также `calibration_decay` в `UncertaintyConfig` нигде не используется.

### 17.9. `current()` и `diagnostics()` не встроены в runtime

Публичный API есть, но:

- pipeline их не вызывает;
- risk/decision/execution их не используют;
- реальный consumer только unit-тест.

### 17.10. Nullable logger заявлен, но реально не поддерживается

В `uncertainty_engine.hpp` комментарий говорит, что `logger` допускается nullable.

Но в `uncertainty_engine.cpp` есть безусловные вызовы:

- `logger_->debug(...)`

Следствие:

- при `logger == nullptr` модуль упадёт;
- комментарий и фактическое поведение расходятся.

### 17.11. Значимая часть доступных uncertainty-drivers не задействована

Для futures scalp-бота особенно заметно, что uncertainty не использует:

- `VPIN`
- `book_instability`
- `CUSUM`
- `ATR`
- `book_imbalance`
- `immediate_liquidity`
- `combined_risk_multiplier` из ML

То есть uncertainty пока не fully exploits microstructure-heavy nature of project.

## 18. Что касается spot / futures

Внутри самого `src/uncertainty` и `tests/unit/uncertainty` прямых упоминаний или веток для spot-торговли нет.

Это хорошо: сам uncertainty-модуль не содержит spot-specific логики.

Но есть другой важный факт для futures-only бота:

- uncertainty опирается на execution-mode gating, size scaling и NoTrade veto;
- ошибки тайминга и невозможные thresholds в таком модуле особенно опасны, потому что они влияют не на одну стратегию, а на весь futures execution stack сразу.

## 19. Краткий итог по индикаторам и входам

### Реализовано и реально используется uncertainty

Да, все необходимые входы для **текущей** uncertainty-модели реализованы upstream.

### Реализовано в проекте, но uncertainty не использует

Да, значительная часть feature-space, ML-space и portfolio-space не задействована.

### Следовательно

Правильный итоговый вывод такой:

1. для текущей rule-based uncertainty-модели входное покрытие достаточное;
2. для полной production-grade uncertainty-семантики проекта интеграция пока неполная;
3. в модуле есть несколько серьёзных архитектурных и логических несоответствий: отсутствие YAML-конфига, недостижимый `liquidity_ratio > 3.0`, лаг в cooldown/execution-mode escalation, и мнимая feedback-calibration.

## 20. Краткий вывод одной фразой

Текущий `uncertainty` — это сильный концептуально, но ещё не доведённый до production-grade stateful uncertainty aggregator для USDT-M futures scalp-бота: он глубоко встроен в pipeline и использует 9 размерностей риска, но страдает от отсутствия конфигурирования через YAML, недостижимого execution-threshold по ликвидности, частично мёртвой feedback/diagnostics-логики и неполной интеграции с поздними ML-сигналами текущего тика.