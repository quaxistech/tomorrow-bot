# Модуль opportunity_cost — подробный разбор

Временный рабочий документ.

Дата: 2026-04-10

## 1. Назначение модуля

`src/opportunity_cost` — это portfolio-aware слой отбора торговых возможностей перед sizing, risk и execution.

Его задача не в том, чтобы:

- генерировать торговый сигнал;
- оценивать микроструктуру рынка;
- рассчитывать риск-лимиты;
- отправлять ордер на биржу.

Его задача уже и строже:

1. принять готовое торговое намерение `TradeIntent`;
2. принять оценку стоимости исполнения из `ExecutionAlphaResult`;
3. принять текущий портфельный контекст `PortfolioContext`;
4. посчитать ожидаемую ценность новой возможности;
5. сравнить её с текущей загрузкой капитала и концентрацией;
6. вернуть одно из решений:
   - `Execute`
   - `Defer`
   - `Suppress`
   - `Upgrade`
7. построить полную аудит-трассу, почему принято именно это решение.

В архитектуре это decision/gating-модуль между strategy/execution-alpha и downstream sizing/risk/execution pipeline.

## 2. Состав модуля

В каталоге находятся 5 файлов:

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | сборка библиотеки `tb_opportunity_cost` |
| `opportunity_cost_engine.hpp` | интерфейс и конфигурация движка |
| `opportunity_cost_engine.cpp` | rule-based реализация `evaluate()` и всех правил |
| `opportunity_cost_types.hpp` | типы результата, факторов, причин и портфельного контекста |
| `opportunity_cost_types.cpp` | `to_string()` для action/reason |

Модуль компактный: публичный API фактически состоит из интерфейса `IOpportunityCostEngine`, конкретной реализации `RuleBasedOpportunityCost` и набора DTO-типов результата.

## 3. Сборка и зависимости

`tb_opportunity_cost` собирается как `STATIC` библиотека из двух `.cpp` файлов:

- `opportunity_cost_types.cpp`
- `opportunity_cost_engine.cpp`

Зависимости:

- `tb_common`
- `tb_strategy`
- `tb_execution_alpha`
- `tb_logging`
- `tb_clock`
- `tb_metrics`

Практический смысл зависимостей:

- из `tb_strategy` приходит `TradeIntent`;
- из `tb_execution_alpha` приходит `ExecutionAlphaResult`;
- `ILogger` нужен для structured debug-логирования;
- `IClock` нужен для `computed_at` и измерения latency;
- `IMetricsRegistry` нужен для runtime telemetry;
- `tb_common` даёт strong types вроде `Symbol`, `Timestamp` и т.д.

## 4. Место модуля в боевом runtime-flow

Реальный путь данных для нового входа выглядит так:

1. `StrategyEngine` формирует `TradeIntent`;
2. `ExecutionAlpha` оценивает стиль исполнения и стоимость сделки;
3. `TradingPipeline` собирает `PortfolioContext` из snapshot портфеля;
4. `opportunity_cost_engine_->evaluate(...)` принимает решение по целесообразности входа;
5. если решение разрешает идти дальше, pipeline вызывает:
   - `PortfolioAllocator::compute_size()`;
   - затем `RiskEngine::evaluate()`;
   - затем leverage/execution путь.

То есть opportunity cost находится **до** sizing и **до** risk engine для новых входов.

Важно: для закрытия позиции этот модуль не является обязательным gate. Close path в pipeline идёт по отдельной, более жёсткой и короткой ветке.

## 5. Где именно модуль используется в коде

Основной consumer — `TradingPipeline`.

### 5.1. Создание движка

`TradingPipeline` в конструкторе собирает `OpportunityCostConfig` из общей конфигурации `config_.opportunity_cost` и создаёт:

```cpp
opportunity_cost_engine_ = std::make_shared<opportunity_cost::RuleBasedOpportunityCost>(
    std::move(oc_cfg), logger_, clock_, metrics_);
```

То есть модуль не хардкодится локально, а инициализируется из общей YAML-конфигурации системы.

### 5.2. Вызов в боевой ветке

В `TradingPipeline::on_feature_snapshot()` opportunity cost вызывается только для **новых входов**.

Логика такая:

1. если есть pending close-order, новые входы блокируются ещё раньше;
2. pipeline собирает `PortfolioContext` из `portfolio_->snapshot()`;
3. вызывает `opportunity_cost_engine_->evaluate(...)`;
4. при `Suppress` — вход немедленно отклоняется;
5. при `Defer` — вход откладывается;
6. при `Execute` — pipeline продолжает путь к sizing;
7. при `Upgrade` — pipeline логирует рекомендацию и **пока продолжает как обычный Execute**.

Это важный нюанс: `Upgrade` сейчас означает не полноценную реализацию “закрыть худшую позицию и заменить её”, а рекомендацию более высокого уровня, после которой pipeline пока не делает автоматическую замену внутри этого же шага.

## 6. Входной контракт модуля

Публичный вход один:

```cpp
OpportunityCostResult evaluate(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const PortfolioContext& portfolio_ctx,
    double conviction_threshold
);
```

На первый взгляд вход широк, но реально модуль использует лишь часть полей.

### 6.1. Что используется из `TradeIntent`

По факту в модуле используются:

- `intent.conviction`
- `intent.urgency`
- `intent.symbol` — только для логирования

Остальные поля `TradeIntent` модуль **не использует напрямую**, включая:

- `suggested_quantity`
- `limit_price`
- `signal_intent`
- `trade_side`
- `entry_score`
- `reason_codes`
- `generated_at`

Следствие: opportunity cost сейчас оценивает не “сделку заданного размера”, а абстрактную возможность входа как таковую. Модуль **size-agnostic**.

### 6.2. Что используется из `ExecutionAlphaResult`

Из результата execution-alpha модуль реально использует только одно поле:

```cpp
exec_alpha.quality.total_cost_bps
```

Не используются:

- `recommended_style`
- `urgency_score`
- `fill_probability`
- `adverse_selection_risk`
- `recommended_limit_price`
- `slice_plan`
- `should_execute`
- `decision_factors`

Следствие: opportunity cost воспринимает execution-alpha прежде всего как источник **стоимости исполнения в bps**, а не как полноценный execution-decision object.

### 6.3. Что используется из `PortfolioContext`

Используются:

- `gross_exposure_pct`
- `symbol_exposure_pct`
- `strategy_exposure_pct`
- `current_drawdown_pct`
- `consecutive_losses`
- `has_worst_position`
- `worst_position_net_bps`

Не используются напрямую:

- `net_exposure_pct`
- `available_capital`
- `total_capital`
- `open_positions_count`
- `worst_position_symbol`

Это важный архитектурный факт: DTO шире, чем реально используемая логика.

### 6.4. Что такое `conviction_threshold`

Это внешний порог, который модуль получает извне, обычно из `config_.decision.min_conviction_threshold`.

Opportunity cost не владеет этим параметром сам, а адаптирует его под drawdown и серию убытков.

## 7. Выходной контракт

Результат работы — `OpportunityCostResult`.

Он содержит:

- `score` — полная декомпозиция scoring;
- `action` — итоговое решение;
- `reason` — основная причина;
- `rank` — сейчас всегда `1`;
- `budget_utilization` — текущая загрузка бюджета;
- `rationale` — человекочитаемое пояснение;
- `reason_codes` — машиночитаемые коды;
- `factors` — структурированная аудит-трасса;
- `computed_at` — timestamp вычисления.

### 7.1. `OpportunityAction`

Варианты решения:

- `Execute` — исполнять сейчас;
- `Defer` — идея не плохая, но сейчас не лучший момент;
- `Suppress` — вход не стоит капитала;
- `Upgrade` — кандидат лучше худшей текущей позиции.

### 7.2. `OpportunityReason`

Причины более детальны, чем action:

- `NegativeNetEdge`
- `ConvictionBelowThreshold`
- `HighExposureLowConviction`
- `InsufficientNetEdge`
- `CapitalExhausted`
- `HighConcentration`
- `StrongEdgeAvailable`
- `HighConvictionOverride`
- `UpgradeBetterCandidate`
- `DefaultDefer`

Именно это делает модуль удобным для аудита и логирования.

## 8. Конфигурация `OpportunityCostConfig`

Конфиг делится на несколько групп.

### 8.1. Пороги net edge

- `min_net_expected_bps`
- `execute_min_net_bps`

Первый порог определяет, ниже какого net edge идея вообще подавляется. Второй — когда её уже можно исполнять немедленно.

### 8.2. Пороги экспозиции

- `high_exposure_threshold`
- `high_exposure_min_conviction`

Они управляют тем, насколько охотно система продолжает открывать позиции при уже высокой загрузке капитала.

### 8.3. Пороги концентрации

- `max_symbol_concentration`
- `max_strategy_concentration`

По символу и по стратегии поведение разное: символ может привести к `Suppress`, стратегия — к `Defer`.

### 8.4. Порог капитального истощения

- `capital_exhaustion_threshold`

При его превышении модуль просто блокирует новый вход.

### 8.5. Веса composite score

- `weight_conviction`
- `weight_net_edge`
- `weight_capital_efficiency`
- `weight_urgency`

Validator требует, чтобы их сумма была примерно `1.0`.

### 8.6. Масштаб перевода conviction → bps

- `conviction_to_bps_scale`

Это один из центральных параметров модели: именно он определяет, какой ожидаемый return в bps соответствует conviction уровня `1.0`.

### 8.7. Upgrade и drawdown penalty

- `upgrade_min_edge_advantage_bps`
- `drawdown_penalty_scale`

Первый параметр определяет, насколько новый кандидат должен превосходить худшую текущую позицию. Второй — насколько агрессивно повышается required conviction при просадке.

## 9. Инициализация и runtime telemetry

Конструктор `RuleBasedOpportunityCost` делает две вещи:

1. сохраняет config/logger/clock/metrics;
2. если metrics доступны — регистрирует счётчики, gauge и histogram.

Создаются:

- `opportunity_cost_actions_total{action=execute|defer|suppress|upgrade}`
- `opportunity_cost_net_edge_bps`
- `opportunity_cost_score`
- `opportunity_cost_decision_latency_ns`

Histogram buckets:

- `100`
- `500`
- `1000`
- `5000`
- `10000`
- `50000`
- `100000`

То есть модуль изначально проектировался как очень дешёвый по latency decision step.

## 10. `evaluate()` — центральный метод модуля

Алгоритм `evaluate()` по шагам:

1. берёт mutex;
2. фиксирует `start = clock_->now()`;
3. создаёт `OpportunityCostResult`;
4. выставляет `computed_at = start`;
5. выставляет `budget_utilization = portfolio_ctx.gross_exposure_pct`;
6. считает `score = compute_score(...)`;
7. определяет `(action, reason) = determine_action(...)`;
8. выставляет `rank = 1`;
9. строит `factors = build_factors(...)`;
10. заполняет `reason_codes`;
11. собирает строку `rationale`;
12. обновляет metrics;
13. пишет debug-лог;
14. возвращает результат.

### 10.1. Потокобезопасность

Вся оценка защищена одним `std::mutex`.

Это значит:

- модуль thread-safe;
- несколько параллельных вызовов не порвут shared state метрик/логики;
- но конкурентные вызовы сериализуются.

Поскольку внутри нет тяжёлых вычислений, такая схема выглядит разумно.

## 11. `compute_score()` — как модуль оценивает привлекательность возможности

Это самая математическая часть модуля.

### 11.1. Expected return из conviction

Код делает:

```cpp
conviction_pow = pow(clamp(conviction, 0, 1), 1.5)
expected_return_bps = conviction_pow * conviction_to_bps_scale
```

Практический смысл:

- conviction сначала ограничивается диапазоном `[0, 1]`;
- затем нелинейно усиливается степенью `1.5`;
- потом масштабируется в basis points.

Нелинейность важна: сильные сигналы отделяются от средних быстрее, чем при линейной зависимости.

### 11.2. Execution cost

Код берёт:

```cpp
execution_cost_bps = exec_alpha.quality.total_cost_bps
```

То есть cost здесь полностью делегирован execution-alpha модулю.

### 11.3. Net expected edge

```cpp
net_expected_bps = expected_return_bps - execution_cost_bps
```

Это главный показатель, который затем реально участвует в rule-based decision.

### 11.4. Capital efficiency

```cpp
exposure_floor = max(gross_exposure_pct, 0.05)
capital_efficiency = net_expected_bps / exposure_floor
```

Смысл:

- при низкой текущей загрузке капитала используется мягкий пол `5%`;
- это защищает от деления на ноль;
- одновременно это слегка стимулирует новые входы в недозагруженном портфеле.

### 11.5. Нормализованные компоненты score

Код формирует:

- `conviction_component`
- `net_edge_component`
- `capital_efficiency_component`
- `urgency_component`

Каждый компонент clamp-ится в `[0, 1]`.

### 11.6. Composite score

Итоговая формула:

```cpp
score = clamp(
    w_conviction * conviction_component +
    w_net_edge * net_edge_component +
    w_cap_eff * capital_efficiency_component +
    w_urgency * urgency_component,
    0, 1)
```

Это аккуратный, прозрачный, explainable scoring.

## 12. Самый важный нюанс scoring

`score.score` почти не участвует в решении напрямую.

Это ключевой инженерный факт.

`determine_action()` не использует итоговый composite score как главный decision variable. Для выбора action реально используются в основном:

- `score.net_expected_bps`
- `conviction`
- портфельные лимиты и концентрации
- наличие худшей позиции

Следствие:

- composite score здесь скорее объясняющий и телеметрический;
- rule-based gating важнее, чем ranking по суммарному score;
- модуль больше похож на rule engine с audit scoring, чем на полноценный score-driven ranker.

## 13. `determine_action()` — полный порядок правил

Это ядро policy-логики.

Порядок правил важен, потому что решение принимается по **первому сработавшему** условию.

### Правило 1. Отрицательный или слишком низкий net edge

Если:

```cpp
score.net_expected_bps < min_net_expected_bps
```

то решение:

- `Suppress`
- reason = `NegativeNetEdge`

### Правило 2. Conviction ниже эффективного порога

Если:

```cpp
conviction < effective_conviction_threshold(...)
```

то:

- `Suppress`
- `ConvictionBelowThreshold`

### Правило 3. Капитал исчерпан

Если:

```cpp
gross_exposure_pct > capital_exhaustion_threshold
```

то:

- `Suppress`
- `CapitalExhausted`

### Правило 4. Слишком высокая концентрация по символу

Если:

```cpp
symbol_exposure_pct > max_symbol_concentration
```

то:

- `Suppress`
- `HighConcentration`

### Правило 5. Слишком высокая концентрация по стратегии

Если:

```cpp
strategy_exposure_pct > max_strategy_concentration
```

то:

- `Defer`
- `HighConcentration`

### Правило 6. Высокая экспозиция + недостаточная conviction

Если:

```cpp
gross_exposure_pct > high_exposure_threshold &&
conviction < high_exposure_min_conviction
```

то:

- `Defer`
- `HighExposureLowConviction`

### Правило 7. Upgrade better candidate

Если:

- `has_worst_position == true`
- новый кандидат лучше худшей позиции больше чем на `upgrade_min_edge_advantage_bps`

то:

- `Upgrade`
- `UpgradeBetterCandidate`

### Правило 8. Достаточный edge для немедленного исполнения

Если:

```cpp
net_expected_bps > execute_min_net_bps &&
gross_exposure_pct < high_exposure_threshold
```

то:

- `Execute`
- `StrongEdgeAvailable`

### Правило 9. High conviction override

Если:

```cpp
conviction >= high_exposure_min_conviction &&
net_expected_bps > 0.0
```

то:

- `Execute`
- `HighConvictionOverride`

Комментарий в коде прямо подчёркивает: этот override **не должен переезжать отрицательный edge**, потому что Rule 1 уже отработал раньше.

### Правило 10. Положительный, но недостаточный edge

Если:

```cpp
net_expected_bps > 0.0
```

то:

- `Defer`
- `InsufficientNetEdge`

### Правило 11. Default defer

Если ничего выше не сработало:

- `Defer`
- `DefaultDefer`

## 14. Что важно в порядке правил

Из-за порядка правил поведение неочевидно на пограничных кейсах.

### 14.1. `NegativeNetEdge` сильнее, чем low conviction

Если сигнал и так отрицателен по net edge, то модуль вернёт `NegativeNetEdge`, даже если conviction тоже низкий.

### 14.2. `HighConvictionOverride` не override-ит всё подряд

Он не может переехать:

- отрицательный net edge;
- capital exhausted;
- symbol concentration;
- strategy concentration;

потому что эти правила стоят раньше.

### 14.3. `Upgrade` приоритетнее обычного `Execute`

Если есть худшая позиция и новый кандидат сильно лучше её, модуль отдаст `Upgrade`, даже если кандидат и так легко проходит обычное `Execute`.

## 15. `effective_conviction_threshold()`

Это адаптивная надстройка над внешним base threshold.

Формула:

1. стартуем с `base_threshold`;
2. если есть drawdown, добавляем:

```cpp
drawdown_penalty_scale * (current_drawdown_pct / 5.0)
```

3. если есть серия убытков, добавляем:

```cpp
0.02 * consecutive_losses
```

4. clamp в диапазон `[0.0, 0.95]`.

### 15.1. Практический смысл drawdown penalty

При дефолтном `drawdown_penalty_scale = 0.5` каждые 5% просадки добавляют `+0.5` к требуемому conviction.

Это очень агрессивный penalty.

Например:

- base threshold = `0.30`
- drawdown = `10%`

тогда добавка = `0.5 * (10 / 5) = 1.0`, и итоговый threshold clamp-ится к `0.95`.

То есть в дефолтной модели уже при умеренной просадке модуль практически перестаёт пускать новые сигналы без экстремально высокой conviction.

### 15.2. Практический смысл серии убытков

Каждый loss streak поднимает threshold на `0.02`.

Это мягче, чем drawdown penalty, но всё равно заметно ограничивает входы после серии неудач.

## 16. `build_factors()` — аудит решения

После того как решение принято, модуль строит `OpportunityCostFactors`.

Туда попадают:

- исходные входные данные (`conviction`, `urgency`, `execution_cost_bps`, `exposure_pct`, `effective threshold`);
- `rule_id`, который реально сработал;
- `reason`;
- `would_be_without_exposure`;
- флаги `concentration_limited`, `capital_limited`, `drawdown_penalized`.

### 16.1. `rule_id`

Это удобная production-фича: решение можно агрегировать не только по reason string, но и по номеру правила.

### 16.2. Counterfactual `would_be_without_exposure`

Модуль пытается ответить на вопрос:

“Что бы произошло, если бы проблема была только в загрузке капитала?”

Но важно понимать ограничение: этот counterfactual строится **не по полному повторному прогону правил**, а только по `net_expected_bps` и порогам execute/min.

Следствие: это полезный, но упрощённый counterfactual, а не точная симуляция альтернативного решения.

## 17. Логирование и reason codes

После вычисления результата модуль делает две формы explainability.

### 17.1. `reason_codes`

По умолчанию туда кладутся:

- `to_string(action)`
- `to_string(reason)`

Дополнительно, если применимо:

- `ConcentrationLimited`
- `CapitalLimited`
- `DrawdownPenalized`

### 17.2. `rationale`

Это строка вида:

```text
action=Execute reason=StrongEdgeAvailable rule=8 net_bps=... score=... efficiency=... exposure=...
```

То есть у результата есть и machine-readable, и human-readable форма.

## 18. Какие поля результата сейчас скорее заглушки

Есть два поля, которые пока выглядят как архитектурный задел, а не полностью реализованная функциональность.

### 18.1. `rank`

В `evaluate()` всегда ставится:

```cpp
result.rank = 1;
```

Это означает, что модуль пока оценивает **одну** возможность за раз и не делает реального сравнения нескольких кандидатов внутри себя.

### 18.2. `budget_utilization`

Это просто копия:

```cpp
portfolio_ctx.gross_exposure_pct
```

То есть не отдельная расчётная сущность, а репортинговое поле.

## 19. Как pipeline формирует `PortfolioContext`

Это важный runtime-аспект, потому что именно здесь в opportunity cost приходят реальные данные портфеля.

Pipeline делает следующее:

### 19.1. Валовая экспозиция

```cpp
oc_ctx.gross_exposure_pct = port_snap.exposure.exposure_pct / 100.0;
```

`PortfolioEngine` хранит `exposure_pct` как проценты `0..100`, а opportunity cost работает с долями `0..1`, поэтому pipeline делит на `100`.

### 19.2. Чистая экспозиция

```cpp
oc_ctx.net_exposure_pct = net_exposure / total_capital
```

Но сам opportunity cost сейчас это поле не использует.

### 19.3. Концентрация по символу и стратегии

Pipeline проходит по открытым позициям и считает:

- суммарный notional по символу;
- суммарный notional по стратегии.

Затем для фьючерсов делит это на leverage и на total capital, получая **margin-based concentration**, а не raw notional-based concentration.

Это правильно для USDT-M futures контекста проекта.

## 20. Очень важное инженерное наблюдение: mismatch единиц drawdown

Здесь обнаруживается важная интеграционная проблема.

### 20.1. Что говорит сам модуль

В `PortfolioContext` комментарий такой:

```cpp
double current_drawdown_pct;  ///< Текущая просадка (%)
```

И сама логика модуля использует это именно как проценты:

- `drawdown_penalized = current_drawdown_pct > 5.0`
- penalty = `drawdown_penalty_scale * (current_drawdown_pct / 5.0)`

То есть модуль ожидает, что:

- `5.0` означает `5%`
- `10.0` означает `10%`

### 20.2. Что реально делает pipeline

Pipeline передаёт:

```cpp
oc_ctx.current_drawdown_pct = port_snap.pnl.current_drawdown_pct / 100.0;
```

А `port_snap.pnl.current_drawdown_pct` уже и так хранится в процентах.

Следовательно:

- `10%` drawdown превращается в `0.10`;
- модуль видит не `10.0`, а `0.10`.

### 20.3. Практическое следствие

В боевом runtime:

- `drawdown_penalized` почти никогда не станет `true`, потому что сравнение идёт с `5.0`;
- drawdown penalty становится в 100 раз слабее, чем задумано.

Например, при реальной просадке `10%`:

- ожидалось: `10 / 5 = 2.0` шага penalty;
- реально приходит `0.10 / 5 = 0.02` шага.

Это не проблема самого модуля в изоляции, а интеграционный mismatch между pipeline и `opportunity_cost`.

## 21. Production-конфиг модуля

В `configs/production.yaml` opportunity cost настроен очень агрессивно в сторону “почти всё пропускать”.

Текущие production значения:

- `min_net_expected_bps: 0.0`
- `execute_min_net_bps: 0.0`
- `high_exposure_threshold: 0.99`
- `high_exposure_min_conviction: 0.25`
- `max_symbol_concentration: 1.0`
- `max_strategy_concentration: 1.0`
- `capital_exhaustion_threshold: 0.99`
- `weight_conviction: 0.40`
- `weight_net_edge: 0.30`
- `weight_capital_efficiency: 0.15`
- `weight_urgency: 0.15`
- `conviction_to_bps_scale: 200.0`
- `upgrade_min_edge_advantage_bps: 2.0`
- `drawdown_penalty_scale: 0.05`

### 21.1. Практический смысл production-конфига

Этот конфиг делает модуль существенно менее консервативным, чем дефолты в коде:

- практически любой положительный edge уже может пройти;
- высокая загрузка капитала почти не блокирует;
- концентрационные ограничения фактически отключены;
- upgrade можно рекомендовать при очень малом преимуществе;
- drawdown penalty почти выключен.

То есть в production модуль работает не как жёсткий стопор, а как мягкий gating/routing слой.

## 22. Конфиг-валидация

`ConfigValidator::validate_opportunity_cost()` проверяет:

- `execute_min_net_bps >= min_net_expected_bps`;
- все пороги долей в `(0, 1]`;
- веса не отрицательны;
- сумма весов примерно равна `1.0`;
- `conviction_to_bps_scale > 0`;
- `upgrade_min_edge_advantage_bps >= 0`;
- `drawdown_penalty_scale >= 0`.

Это означает, что конфиг модуля хорошо защищён от грубых ошибок формы, хотя не защищён от стратегически слишком агрессивных значений.

## 23. Тестовое покрытие

У модуля есть отдельный unit test файл:

- `tests/unit/opportunity_cost/opportunity_cost_test.cpp`

Тесты реально существуют и проходят.

По текущему состоянию:

- `20` test cases;
- `58` assertions;
- отдельный таргет `test_opportunity_cost`.

### 23.1. Что покрыто тестами

Покрыты:

- базовые решения `Execute/Defer/Suppress`;
- `Upgrade`;
- `HighConvictionOverride`;
- влияние drawdown и consecutive losses;
- корректность `to_string()`;
- decomposition `score`;
- заполнение audit trail;
- monotonicity по conviction;
- `NaN/Inf` safety при нулевом капитале;
- влияние конфигурируемых weights.

### 23.2. Что покрыто слабее или не покрыто

Почти не покрыты напрямую:

- runtime metrics emission;
- потокобезопасность под конкурентной нагрузкой;
- интеграция с `TradingPipeline`;
- mismatch единиц `current_drawdown_pct`;
- то, что `Upgrade` в pipeline пока не приводит к автоматической замене худшей позиции;
- то, что engine игнорирует большую часть полей `TradeIntent`, `ExecutionAlphaResult` и `PortfolioContext`.

## 24. Главные инженерные наблюдения

### 24.1. Это не ranker многих кандидатов, а gate для одного кандидата

Несмотря на наличие `score` и `rank`, модуль сейчас обрабатывает одну возможность за вызов.

### 24.2. Composite score — вторичен относительно rule-based policy

Итоговый `score.score` полезен для explainability, но не является основным decision driver.

### 24.3. Модуль size-agnostic

Размер предполагаемой позиции не входит в формулу. Для скальперского USDT-M пайплайна это означает, что opportunity cost решает вопрос “входить ли вообще”, а не “стоит ли входить именно таким объёмом”.

### 24.4. Модуль почти не использует богатство `ExecutionAlphaResult`

Все execution-нюансы сводятся к `total_cost_bps`. Это упрощает reasoning, но теряет часть информации о fill probability, toxicity и стиле исполнения.

### 24.5. Upgrade пока архитектурно неполный

Сам модуль умеет рекомендовать `Upgrade`, но фактическая замена худшей позиции в том же decision step пока не реализована целиком в pipeline.

### 24.6. Drawdown mismatch — самое важное наблюдение по интеграции

Если смотреть не только на unit-тесты модуля, а на реальный pipeline, то именно здесь находится главная семаническая проблема текущей реализации.

### 24.7. Production-конфиг делает модуль очень мягким

При текущем `production.yaml` opportunity cost в бою выглядит не как строгий риск-фильтр, а как мягкий и довольно permissive селектор.

## 25. Краткий итог

`src/opportunity_cost` — это компактный rule-based decision layer, который оценивает новую торговую возможность с учётом:

- conviction сигнала;
- стоимости исполнения;
- текущей загрузки капитала;
- концентрации портфеля;
- drawdown и loss streak.

Он хорошо встроен в архитектуру:

- создаётся из общей конфигурации;
- вызывается из `TradingPipeline` в правильном месте перед sizing/risk;
- возвращает explainable результат с action/reason/factors;
- имеет валидатор конфигурации и отдельные unit-тесты.

При этом глубокий анализ показывает важные нюансы:

- composite score здесь не является главным decision variable;
- модуль не учитывает размер сделки;
- использует из execution-alpha только total cost;
- `Upgrade` пока не материализуется в полноценную ротацию позиции;
- есть интеграционный mismatch по единицам drawdown между pipeline и самим модулем.

В текущем виде модуль лучше всего понимать как **explainable rule-based gate для новых входов в USDT-M futures pipeline**, а не как полноценный многокандидатный optimizer капитала.