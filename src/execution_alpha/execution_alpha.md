# Подробный разбор модуля execution alpha

Временный аналитический документ.

Источник разбора: текущая реализация `src/execution_alpha`, её реальные вызовы из `pipeline`, использование в `execution planner` и `execution engine`, а также профильные unit-тесты на момент 2026-04-09.
Это описание фактической реализации, а не целевой архитектуры.

## 1. Что входит в модуль

Модуль `execution_alpha` состоит из пяти файлов:

| Файл | Назначение |
|---|---|
| `src/execution_alpha/CMakeLists.txt` | Сборка библиотеки `tb_execution_alpha` |
| `src/execution_alpha/execution_alpha_types.hpp` | Доменные типы результата: стиль исполнения, качество, slicing plan, audit factors |
| `src/execution_alpha/execution_alpha_types.cpp` | `to_string()` для `ExecutionStyle` |
| `src/execution_alpha/execution_alpha_engine.hpp` | Интерфейс `IExecutionAlphaEngine` и реализация `RuleBasedExecutionAlpha` |
| `src/execution_alpha/execution_alpha_engine.cpp` | Реальная rule-based логика оценки исполнения |

Зависимости модуля по CMake:

- `tb_common`
- `tb_features`
- `tb_strategy`
- `tb_logging`
- `tb_clock`
- `tb_metrics`
- `tb_uncertainty`

Практически весь runtime-модуль сосредоточен в одном классе: `RuleBasedExecutionAlpha`.

## 2. Главная роль модуля

`execution alpha` отвечает за предторговую оценку **как именно** исполнять уже одобренный сигнал на USDT-M futures:

1. можно ли вообще исполнять сигнал в текущей микроструктуре;
2. какой стиль исполнения предпочтителен;
3. насколько срочно надо входить;
4. какова ожидаемая стоимость исполнения;
5. нужна ли лимитная цена;
6. стоит ли резать ордер на слайсы.

Иначе говоря, модуль не решает **входить или не входить по стратегии** и не решает **финальный risk approval**. Он решает, как превратить уже сформированный `TradeIntent` в осмысленную execution-рекомендацию с учётом:

- спреда;
- токсичности потока;
- VPIN;
- дисбаланса стакана;
- волатильности;
- моментума;
- CUSUM regime shift;
- uncertainty режима.

## 3. Основные типы данных

### 3.1. `ExecutionStyle`

Поддерживаются 5 стилей исполнения:

- `Passive` — пассивный лимитный ордер в роли maker;
- `Aggressive` — немедленное исполнение, фактически taker/market;
- `Hybrid` — промежуточный режим между passive и aggressive;
- `PostOnly` — только maker, без пересечения стакана;
- `NoExecution` — рынок слишком токсичен, исполнять нельзя.

### 3.2. `ExecutionQualityEstimate`

Это агрегированная оценка качества исполнения:

- `spread_cost_bps`;
- `estimated_slippage_bps`;
- `fill_probability`;
- `adverse_selection_risk`;
- `total_cost_bps`.

Эта структура затем используется ниже по pipeline как часть общей execution/risk картины.

### 3.3. `SlicePlan`

Если ордер считается крупным относительно доступной глубины, модуль может рекомендовать нарезку:

- `num_slices`;
- `slice_interval_ms`;
- `first_slice_pct`;
- `rationale`.

Это не сам TWAP executor, а только рекомендация для его активации.

### 3.4. `DecisionFactors`

Это важнейшая структура explainability/audit:

- компоненты токсичности;
- directional imbalance;
- разложение срочности по компонентам;
- флаги использования VPIN, weighted mid и полноты фич.

Фактически это внутренний audit trail того, **почему** движок выбрал конкретный стиль исполнения.

### 3.5. `ExecutionAlphaResult`

Главный выход модуля содержит:

- `recommended_style`;
- `urgency_score`;
- `quality`;
- `recommended_limit_price`;
- `slice_plan`;
- `should_execute`;
- `rationale`;
- `computed_at`;
- `decision_factors`.

## 4. Интерфейс и конфигурация

### 4.1. `IExecutionAlphaEngine`

Публичный интерфейс очень узкий: один метод

`evaluate(intent, features, uncertainty)`

На вход подаются:

- `strategy::TradeIntent`;
- `features::FeatureSnapshot`;
- `uncertainty::UncertaintySnapshot`.

Это подчёркивает архитектурную роль модуля: он чисто аналитический и не хранит собственное торговое состояние между вызовами.

### 4.2. `RuleBasedExecutionAlpha::Config`

Конфиг делится на несколько смысловых блоков.

#### Базовые пороги

- `max_spread_bps_passive`;
- `max_spread_bps_any`;
- `adverse_selection_threshold`;
- `urgency_passive_threshold`;
- `urgency_aggressive_threshold`;
- `large_order_slice_threshold`.

#### VPIN

- `vpin_toxic_threshold`;
- `vpin_weight`.

#### Дисбаланс стакана

- `imbalance_favorable_threshold`;
- `imbalance_unfavorable_threshold`.

#### Ценообразование

- `use_weighted_mid_price`;
- `limit_price_passive_bps`.

#### Срочность

- `urgency_cusum_boost`;
- `urgency_tod_weight`.

#### Fill probability и PostOnly

- `min_fill_probability_passive`;
- `postonly_spread_threshold_bps`;
- `postonly_urgency_max`;
- `postonly_adverse_max`.

Важно: этот конфиг не зашит в конструкторе `TradingPipeline`, а прокидывается из `config_.execution_alpha` и проходит отдельную валидацию в `ConfigValidator`.

## 5. Какие данные модуль реально использует

### 5.1. Из `TradeIntent`

Основные поля, влияющие на решение:

- `side`;
- `suggested_quantity`;
- `urgency`;
- `limit_price`;
- `snapshot_mid_price`;
- косвенно `signal_intent`/`trade_side` downstream, но не внутри самого движка.

### 5.2. Из `FeatureSnapshot`

Реально используются не все признаки, а конкретный поднабор:

#### Technical

- `volatility_valid`, `volatility_5`;
- `momentum_valid`, `momentum_5`;
- `cusum_valid`, `cusum_regime_change`;
- `tod_valid`, `tod_alpha_score`.

#### Microstructure

- `spread`, `spread_bps`, `spread_valid`;
- `book_imbalance_5`, `book_imbalance_valid`;
- `weighted_mid_price`;
- `aggressive_flow`, `trade_flow_valid`;
- `bid_depth_5_notional`, `ask_depth_5_notional`, `liquidity_valid`;
- `book_instability`, `instability_valid`;
- `vpin`, `vpin_valid`, `vpin_toxic`.

#### Execution context

- `estimated_slippage_bps`.

### 5.3. Из `UncertaintySnapshot`

Используются:

- `level`;
- `execution_mode`;
- `size_multiplier`;
- `aggregate_score` — в логах/обосновании блокировок.

## 6. Как работает `evaluate()` пошагово

### 6.1. Валидация минимального качества данных

`validate_features()` проверяет только три вещи:

- `mid_price > 0`;
- `spread_valid == true`;
- `spread_bps >= 0`.

Если эти условия не выполнены:

- `should_execute = false`;
- `recommended_style = NoExecution`;
- `decision_factors.features_complete = false`.

Практический смысл: движок не требует полного набора advanced features, а работает в режиме graceful degradation.

### 6.2. Жёсткие блокировки по uncertainty

Далее идут ранние stop-условия:

1. `HaltNewEntries` → `NoExecution`;
2. `DefensiveOnly` + `intent.side == Buy` → `NoExecution`;
3. `UncertaintyLevel::Extreme` → `NoExecution`.

Здесь важно: логика `DefensiveOnly` проверяет именно `side == Buy`, а не `signal_intent == LongEntry/ShortEntry` и не `trade_side == Open`. Для фьючерсов это означает, что **новый short entry (`Sell` + `Open`) этим условием не блокируется внутри execution alpha**.

### 6.3. Расчёт срочности

Базовая формула внутри `compute_urgency()`:

$$
urgency = clamp(base + vol\_adj + momentum\_adj + cusum\_adj + tod\_adj, 0, 1)
$$

Где:

- `base = intent.urgency`;
- `vol_adj` растёт с `volatility_5`;
- `momentum_adj` положителен, если цена уходит против нас;
- `cusum_adj = urgency_cusum_boost`, если обнаружен regime shift;
- `tod_adj = tod_alpha_score * urgency_tod_weight`.

После этого срочность масштабируется uncertainty-множителем:

$$
urgency\_final = urgency \cdot uncertainty.size\_multiplier
$$

То есть при повышенной неопределённости модуль не только режет размер позиции через другие слои, но и дополнительно делает исполнение менее агрессивным.

### 6.4. Adverse selection score

`estimate_adverse_selection()` строит взвешенный скор токсичности по доступным компонентам.

Используются:

- VPIN;
- aggressive flow;
- book instability;
- spread as toxicity proxy.

Нормализация VPIN:

$$
vpin\_norm = clamp\left(\frac{vpin}{vpin\_toxic\_threshold}, 0, 1\right)
$$

Итоговый adverse score:

$$
adverse = \frac{vpin\_norm \cdot vpin\_weight + flow + instability + spread\_score}{vpin\_weight + N}
$$

где `N` — число остальных реально доступных факторов.

Если какая-то фича невалидна, она просто исключается из расчёта.

### 6.5. Directional imbalance

`get_directional_imbalance()` переводит общий `book_imbalance_5` в знак относительно направления сделки:

- для `Buy`: положительный дисбаланс означает ситуацию в нашу пользу;
- для `Sell`: знак инвертируется.

Итоговый диапазон:

$$
[-1, +1]
$$

где `-1` — против нас, `+1` — в нашу пользу.

### 6.6. Выбор стиля исполнения

`determine_style()` применяет правила в фиксированном порядке.

#### Правило 1. Спред слишком широкий

Если:

$$
spread\_bps > max\_spread\_bps\_any
$$

то результат сразу `NoExecution`.

#### Правило 2. VPIN hard stop

Если `vpin_valid && vpin_toxic && vpin > vpin_toxic_threshold`, модуль сразу запрещает исполнение.

#### Правило 3. Aggregate toxicity hard stop

Если:

$$
adverse > adverse\_selection\_threshold
$$

то тоже `NoExecution`.

#### Правило 4. Высокая срочность

Если:

$$
urgency > urgency\_aggressive\_threshold
$$

то выбирается `Aggressive`.

#### Правило 5. Идеальные maker-условия

Если одновременно:

- очень узкий спред;
- очень низкая срочность;
- низкая токсичность;
- нет токсичного VPIN;

то выбирается `PostOnly`.

#### Правило 6. Passive с учётом имбаланса

Если срочность низкая и спред подходит для passive, модуль выбирает `Passive`, но при сильном directional imbalance против нас переключается на `Hybrid`.

#### Правило 7. Умеренная срочность + хороший imbalance

Если срочность не агрессивная, а directional imbalance в нашу пользу, модуль снова может предпочесть `Passive`.

#### Фоллбек

Во всех прочих случаях выбирается `Hybrid`.

### 6.7. Дополнительный downgrade при High uncertainty

После выбора стиля есть ещё одно правило:

если `uncertainty.level == High` и результат был `Aggressive`, модуль понижает его до `Passive`.

Это важная защитная логика: даже при высокой срочности execution alpha перестраховывается в шумном рынке.

### 6.8. Оценка fill probability

`estimate_fill_probability()` устроен по стилям.

#### `Aggressive`

Возвращается фиксированное значение `0.95`.

#### `Passive` / `PostOnly`

База:

$$
fp = 0.60
$$

Дальше:

- штраф за широкий спред;
- штраф за крупный размер ордера относительно доступной глубины;
- бонус/штраф за directional imbalance.

Итог clamp:

$$
fp \in [min\_fill\_probability\_passive, 0.75]
$$

#### `Hybrid`

База:

$$
fp = 0.80
$$

с умеренным штрафом за спред и clamp в диапазоне `[0.60, 0.92]`.

#### `NoExecution`

`fill_probability = 0`.

### 6.9. Оценка стоимости исполнения

`estimate_quality()` формирует `ExecutionQualityEstimate`.

Для passive/post-only:

$$
total\_cost\_bps = spread\_cost\_bps \cdot 0.30 + adverse \cdot spread\_cost\_bps \cdot 0.20
$$

Для aggressive/hybrid:

$$
total\_cost\_bps = spread\_cost\_bps + estimated\_slippage\_bps + adverse \cdot spread\_cost\_bps \cdot 0.15
$$

Смысл:

- passive дешевле по прямой стоимости;
- aggressive платит spread + slippage;
- adverse selection увеличивает penalty в обеих ветках.

### 6.10. Расчёт лимитной цены

`compute_limit_price()` возвращает цену только для `Passive`, `PostOnly` и `Hybrid`.

#### Опорная цена

Если включён `use_weighted_mid_price` и есть `weighted_mid_price`, он используется вместо обычного `mid_price`.

#### Passive / PostOnly

Пусть:

$$
half\_spread = \frac{spread}{2}
$$

$$
raw\_improvement = mid \cdot \frac{limit\_price\_passive\_bps}{10000}
$$

$$
capped\_improvement = min(raw\_improvement, 0.30 \cdot half\_spread)
$$

Тогда:

- для `Buy`:

$$
limit = mid - half\_spread + capped\_improvement
$$

- для `Sell`:

$$
limit = mid + half\_spread - capped\_improvement
$$

#### Hybrid

Используется внутренняя цена внутри спреда:

$$
inside\_offset = 0.4 \cdot half\_spread
$$

- `Buy`: `mid - inside_offset`
- `Sell`: `mid + inside_offset`

### 6.11. План нарезки

`compute_slice_plan()` рекомендует slicing только если есть валидная ликвидность.

Доступная глубина берётся так:

- для `Buy`: `bid_depth_5_notional`;
- для `Sell`: `ask_depth_5_notional`.

Затем:

$$
order\_notional = suggested\_quantity \cdot mid\_price
$$

$$
ratio = \frac{order\_notional}{available\_depth}
$$

Если `ratio < large_order_slice_threshold`, slicing не нужен.

Иначе:

- `num_slices = clamp(max(2, int(ratio / threshold)), 2, 10)`;
- `slice_interval_ms` масштабируется от `200 * num_slices`;
- при высокой нестабильности стакана (`book_instability > 0.5`) интервал ускоряется (`*0.7`);
- `first_slice_pct = 1 / num_slices`.

## 7. Как модуль используется в runtime

### 7.1. Инициализация в `TradingPipeline`

`TradingPipeline` создаёт `RuleBasedExecutionAlpha` из `config_.execution_alpha`, а не через хардкод.

Это важно, потому что production-пороги живут в общей конфигурации системы и валидируются отдельно через `ConfigValidator::validate_execution_alpha()`.

### 7.2. Обычный вход в новую позицию

Для нового entry pipeline делает:

1. нормализует `intent`;
2. подготавливает `snapshot` и `uncertainty`;
3. вызывает `execution_alpha_->evaluate(intent, snapshot, uncertainty)`;
4. передаёт результат в:
   - `opportunity_cost_engine_->evaluate(...)`;
   - затем в `risk_engine_->evaluate(...)`;
   - затем в `execution_engine_->execute(...)`.

То есть execution alpha влияет не только на execution planner, но и на decision/risk chain выше по стеку.

### 7.3. Закрытие позиции

Pipeline всё равно сначала вызывает `evaluate()`, но затем для закрытия принудительно делает:

- `recommended_style = Aggressive`;
- `should_execute = true`;
- `urgency_score = 1.0`.

Практический смысл: close/reduce операции обязаны исполниться, даже если alpha считала рынок плохим для нового входа.

### 7.4. TWAP-слайсы

Если есть активный `SmartTwapExecutor`, каждый следующий слайс снова проходит через `execution_alpha_->evaluate(...)`, но уже с пустым `twap_uncertainty{}`.

Дальше этот слайс всё равно проходит через risk engine перед фактическим исполнением.

### 7.5. Активация TWAP

После основного risk approval pipeline проверяет:

```text
exec_alpha.slice_plan.has_value()
```

Если execution alpha рекомендует slicing, это является одним из триггеров включения `Smart TWAP`.

### 7.6. Использование в execution planner

`ExecutionPlanner` берёт из `ExecutionAlphaResult` прежде всего:

- `recommended_style`;
- `urgency_score`;
- `recommended_limit_price`.

Он переводит стиль alpha в `PlannedExecutionStyle`, а затем в `OrderType` и `TimeInForce`.

### 7.7. Использование в execution engine

`ExecutionEngine::validate_inputs()` дополнительно проверяет:

- `exec_alpha.should_execute`;
- `exec_alpha.recommended_style != NoExecution`.

Если alpha запретила исполнение, execution engine возвращает `ExecutionFailed`.

## 8. Важный runtime-нюанс: часть рекомендаций не доживает до биржи

Хотя execution alpha умеет рекомендовать `Passive`, `Hybrid` и `PostOnly`, текущая live-цепочка имеет существенное ограничение.

### 8.1. Planner частично форсирует market semantics

В `ExecutionPlanner::style_to_order_type()`:

- `PassiveLimit`;
- `SmartFallback`;
- `CancelIfNotFilled`;
- `ReduceOnly`

сейчас мапятся в `OrderType::Market` с комментарием, что private WS ещё не реализован.

### 8.2. Execution engine форсирует market ещё жёстче

В `ExecutionEngine::create_order_record()` тип ордера и `tif` принудительно ставятся как:

- `OrderType::Market`;
- `TimeInForce::ImmediateOrCancel`.

Следствие:

- execution alpha реально влияет на gating, urgency, cost model и TWAP activation;
- но её рекомендации по пассивному лимитному исполнению и post-only сейчас **не полностью реализуются на уровне боевого ордера**.

Это один из ключевых архитектурных выводов по текущему состоянию модуля.

## 9. Покрытие тестами

У модуля есть отдельный unit-набор `tests/unit/execution_alpha/execution_alpha_test.cpp`.

На момент анализа он покрывает 18 сценариев:

- широкий спред;
- высокая и низкая срочность;
- токсичный поток;
- корректность fill probability диапазонов;
- корректность limit price;
- `to_string()`;
- VPIN toxic / VPIN used;
- благоприятный и неблагоприятный imbalance;
- CUSUM boost;
- PostOnly;
- невалидные данные;
- заполнение `DecisionFactors`;
- variation fill probability;
- отсутствие limit price у aggressive;
- использование weighted mid.

По факту проверки:

- 18/18 execution_alpha тестов проходят.

## 10. Сильные стороны текущего дизайна

### 10.1. Есть полноценный explainability слой

`DecisionFactors` делает модуль прозрачным для аудита и дебага. Это сильнее обычного “вернули стиль и разошлись”.

### 10.2. Модуль реально использует микроструктуру

Здесь учитываются:

- VPIN;
- aggressive flow;
- book instability;
- directional imbalance;
- spread;
- depth.

Для фьючерсного скальпинга это правильный уровень детализации.

### 10.3. Есть graceful degradation

Модуль не падает без полного набора advanced features и умеет работать по минимальному набору `mid_price + spread`.

### 10.4. Интеграция с uncertainty корректная по духу

`HaltNewEntries`, `Extreme`, `size_multiplier` и downgrade `Aggressive -> Passive` делают execution alpha защитным слоем, а не просто генератором красивых чисел.

### 10.5. Alpha влияет на TWAP, а не только на order type

Через `slice_plan` модуль влияет на higher-level execution orchestration, а не только на цену/стиль одного ордера.

## 11. Главные ограничения текущей реализации

### 11.1. `DefensiveOnly` блокирует не все новые входы

Внутри `evaluate()` проверка сделана как:

- `execution_mode == DefensiveOnly && intent.side == Buy`

Для USDT-M futures это означает, что `ShortEntry` как `Sell`-вход execution alpha сама по себе не блокирует.

### 11.2. Passive/PostOnly рекомендации частично теряются downstream

Из-за текущей реализации planner/execution engine часть execution-style логики не превращается в реальный limit/post-only ордер на бирже.

### 11.3. Fill probability лишь частично data-driven

В комментариях модуль подаётся как data-driven, но:

- `Aggressive = 0.95` жёстко;
- `Hybrid` стартует от фиксированной базы `0.80`.

То есть модель не полностью эмпирическая, а смесь market heuristics и data-driven поправок.

### 11.4. Cost model не учитывает fees/rebates явно

`estimate_quality()` учитывает spread/slippage/adverse selection, но не моделирует отдельно maker rebate и taker fee в явном виде.

### 11.5. Slice plan не учитывает биржевые фильтры и фактический min notional

`compute_slice_plan()` оценивает только отношение размера к глубине и нестабильность стакана. Биржевые фильтры и минимальные размеры ордера в этом слое не применяются.

### 11.6. Валидация фич минимальна

Это плюс как graceful degradation, но и ограничение: модуль может принять решение при частично пустом наборе сигналов микроструктуры.

## 12. Практический смысл для текущего бота

В текущей архитектуре `execution alpha` — это защитный и координирующий слой между strategy/risk и execution.

Его фактическая production-роль:

- отфильтровывать явно плохие microstructure conditions;
- определять urgency;
- давать quality estimate для downstream-модулей;
- инициировать slicing/TWAP для крупных ордеров;
- давать explainability по execution-решению.

Но это ещё не полностью реализованный smart order router: часть его пассивных рекомендаций в текущем runtime всё ещё сводится к market execution из-за ограничений planner/execution слоя.

## 13. Краткий итог

`execution alpha` — это rule-based execution intelligence модуль для USDT-M futures scalping, который принимает `TradeIntent`, `FeatureSnapshot` и `UncertaintySnapshot`, а на выходе даёт:

- разрешение/запрет на исполнение;
- стиль исполнения;
- срочность;
- стоимость исполнения;
- лимитную цену;
- рекомендацию по slicing.

С инженерной точки зрения модуль уже довольно зрелый:

- есть audit trail;
- есть микроструктурная логика;
- есть VPIN и imbalance;
- есть integration с uncertainty и TWAP;
- есть отдельный unit test suite.

Главное практическое ограничение не внутри самого модуля, а на стыке с downstream execution: execution alpha уже умеет быть умным, но live-order path пока не всегда умеет исполнить его рекомендации как настоящий passive/post-only execution.