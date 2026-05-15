# Execution Module — подробный временный разбор

Дата анализа: 2026-04-09

## 1. Назначение модуля

`src/execution` — это модуль исполнения ордеров, то есть шлюз между торговым pipeline и фактической отправкой заявок. Его задача не генерировать торговую идею, а принять уже подготовленное намерение на сделку и провести его через набор этапов:

1. проверить, что сделка вообще допускается;
2. выбрать способ исполнения;
3. создать и зарегистрировать ордер;
4. зарезервировать капитал или маржу;
5. отправить заявку через абстракцию биржевого submitter-а;
6. обработать fill, отмену, recovery и телеметрию.

Ключевая особенность текущей реализации: модуль архитектурно выглядит как полноценный execution/OMS слой с поддержкой limit, post-only, fallback, partial fill, recovery и TWAP, но в реальном runtime-пути он сильно оптимизирован под быстрые market-исполнения для USDT-M futures.

## 2. Где находится логика и за что отвечает каждый файл

### Корневые файлы

- `execution_engine.hpp` — публичный интерфейс `ExecutionEngine`.
- `execution_engine_new.cpp` — реальная активная реализация `ExecutionEngine`.
- `execution_config.hpp` — конфиг timeouts, slippage-порогов, fallback-поведения и safety-настроек.
- `execution_types.hpp/cpp` — типы уровня planner/execution flow: `ExecutionAction`, `PlannedExecutionStyle`, `ExecutionPlan`, `IntentExecution`, `MarketExecutionContext`.
- `order_types.hpp/cpp` — доменные типы ордера: `OrderRecord`, `FillEvent`, `OrderState`, `PartialFillPolicy`, `OrderSubmitResult`.
- `order_fsm.hpp/cpp` — конечный автомат состояний ордера.
- `order_submitter.hpp` — интерфейс `IOrderSubmitter` и `PaperOrderSubmitter`.
- `twap_executor.hpp/cpp` — отдельный исполнитель Smart TWAP, управляемый pipeline.

### Поддиректории

- `orders/order_registry.*` — централизованный реестр ордеров, FSM и idempotency/dedup state.
- `planner/execution_planner.*` — превращает `TradeIntent` и контекст в конкретный `ExecutionPlan`.
- `fills/fill_processor.*` — применяет fills к ордеру и портфелю.
- `cancel/cancel_manager.*` — отмены ордеров, timeout-cancel, cancel-all.
- `recovery/recovery_manager.*` — reconciliation и перевод ордеров из неопределённого состояния.
- `telemetry/execution_metrics.*` — counters и агрегированные метрики качества исполнения.

### Важная навигационная деталь

В CMake подключён именно `execution_engine_new.cpp`. Старого `execution_engine.cpp` в модуле нет, а вся живая реализация сидит в `execution_engine_new.cpp`.

## 3. Главная сущность: `ExecutionEngine`

`ExecutionEngine` — центральный оркестратор. Он не хранит сложную бизнес-логику внутри себя, а делегирует работу подсистемам:

- `OrderRegistry` — хранение ордеров, FSM, дедупликация интентов, idempotency по fill.
- `ExecutionPlanner` — определение action/style/type/tif/timeout.
- `FillProcessor` — обработка факта исполнения и обновление портфеля.
- `CancelManager` — отмены.
- `RecoveryManager` — восстановление при неопределённом состоянии.
- `ExecutionMetrics` — телеметрия.

Публичный API движка состоит из нескольких групп:

1. `execute(...)` — основная точка входа.
2. `cancel(...)`, `cancel_all_for_symbol(...)`, `cancel_all()` — управление ордерами.
3. `emergency_flatten_symbol(...)` — аварийное закрытие позиции.
4. `on_order_update(...)`, `on_fill_event(...)` — hooks для внешних событий биржи.
5. `active_orders()`, `get_order()`, `orders_for_symbol()`, `cleanup_terminal_orders()` — сервисные методы.
6. `run_reconciliation()` — ручной запуск recovery/reconciliation.
7. `set_leverage(...)` — синхронизация плеча для расчёта reserve amount.

## 4. Сквозной runtime-путь `execute()`

Ниже описан фактический путь ордера по текущему коду.

### Шаг 1. Валидация входов

`validate_inputs(...)` отклоняет сделку, если:

- `RiskDecision.verdict` равен `Denied` или `Throttled`;
- `ExecutionAlphaResult.should_execute == false`;
- `ExecutionAlphaResult.recommended_style == NoExecution`;
- `UncertaintySnapshot.level == Extreme`.

Если uncertainty-кулдаун активен, движок не блокирует ордер автоматически, а только пишет warning.

### Шаг 2. Дедупликация интента

Дедуп-ключ строится как:

```text
correlation_id + ":" + symbol
```

Перед проверкой вызывается очистка устаревших intent-ов по окну `dedup_window_ms`. Если ключ уже встречался, `execute()` возвращает `TbError::IdempotencyDuplicate`.

### Шаг 3. Построение упрощённого market context

`ExecutionEngine` сам синтезирует `MarketExecutionContext` для planner-а:

- если есть `snapshot_mid_price`, то строятся условные `best_bid` и `best_ask` вокруг mid;
- стартовый `spread_bps` по умолчанию выставляется в `2.0`;
- если `exec_alpha.quality.spread_cost_bps > 0`, то спред подменяется им;
- `adverse_selection_risk` переносится из `exec_alpha.quality`.

То есть planner получает не реальный order book, а сильно упрощённую сводку.

### Шаг 4. Построение `ExecutionPlan`

Planner решает:

- какое действие выполняется;
- какой стиль исполнения выбран;
- какой order type и TIF теоретически подходят;
- какая reference/limit цена используется;
- какой timeout нужен;
- нужен ли fallback;
- должен ли ордер быть reduce-only.

Важно: план может быть богатым, но ниже движок всё равно принудительно упрощает его до market-пути.

### Шаг 5. Обработка `NoAction`

Если planner вернул `ExecutionAction::NoAction`, движок не создаёт ордер и завершает `execute()` ошибкой `TbError::ExecutionFailed`.

Это важно: `Hold` здесь трактуется не как успешный no-op, а как отсутствие допустимого ордера.

### Шаг 6. Создание `OrderRecord`

`create_order_record(...)` заполняет базовые поля:

- `order_id` генерируется через `ClientOrderIdGenerator::next()`;
- копируются `symbol`, `side`, `position_side`, `trade_side`, `strategy_id`, `correlation_id`;
- размер берётся из `risk_decision.approved_quantity`;
- `remaining_quantity` изначально равен approved quantity;
- `execution_info.expected_fill_price` берётся из `plan.planned_price`;
- `execution_info.fill_policy` берётся из `default_fill_policy_`.

Но при этом есть жёсткое runtime-правило:

```text
record.order_type = Market
record.tif = ImmediateOrCancel
```

То есть даже если planner рассчитал `PassiveLimit`, `PostOnlyLimit` или `SmartFallback`, сам `ExecutionEngine` в текущем виде всё равно создаёт `Market + IOC`.

### Шаг 7. Регистрация и FSM-переход

Ордер сначала регистрируется в `OrderRegistry`, затем FSM переводится:

```text
New -> PendingAck
```

Если этот переход не удался, ордер считается проваленным.

### Шаг 8. Предварительные проверки перед submit

До вызова биржи движок делает ещё несколько проверок:

1. для market-ордера `price` не должен быть равен нулю;
2. `original_quantity` должен быть больше нуля;
3. должен пройти cash/margin reserve.

Если одна из проверок не проходит, ордер переводится в `Rejected`, причина пишется в `rejection_reason`, а в метриках фиксируется reject.

### Шаг 9. Reserve cash / margin

Reserve применяется только если `trade_side != Close`.

Практический смысл такой:

- новый вход в long резервирует капитал;
- новый вход в short тоже резервирует капитал/маржу;
- закрытие позиции reserve не требует.

Формула резерва:

```text
notional = quantity * price
reserve_amount = notional / leverage
estimated_fee = notional * taker_fee
```

Дополнительно есть проверка `min_notional_usdt`.

Если свободного капитала не хватает, движок пытается автоматически уменьшить размер ордера под доступный cash:

```text
max_notional = available_cash * leverage * 0.95
new_qty = max_notional / price
```

Идея правильная: лучше отправить уменьшенный ордер, чем полностью отказывать.

### Шаг 10. Submit через `IOrderSubmitter`

Сетевой слой полностью вынесен за интерфейс:

- `submit_order(const OrderRecord&)`
- `cancel_order(const OrderId&, const Symbol&)`
- `query_order_fill_price(const OrderId&, const Symbol&)`

Если submit бросает исключение, reserve откатывается, но сам ордер остаётся в registry и потенциально может участвовать в recovery.

Если submit возвращает неуспех, ордер переводится в `Rejected`, reserve освобождается, а `execute()` возвращает ошибку.

### Шаг 11. Успешная отправка

При успехе движок:

1. записывает `exchange_order_id`;
2. фиксирует dedup key как обработанный;
3. пишет лог `Order submitted`;
4. обновляет execution metrics.

### Шаг 12. Немедленная обработка market fill

Так как текущий runtime ориентирован на market execution, после submit движок сразу пытается закрыть цикл исполнения:

1. берёт предварительную цену fill из `order.price`;
2. пытается получить реальную цену через `query_order_fill_price(...)`;
3. определяет фактически отправленный размер через `submitted_quantity`;
4. вызывает `FillProcessor::process_market_fill(...)`.

Именно поэтому большинство ордеров в текущей архитектуре живут очень коротко: практически сразу после отправки они переходят в `Filled`.

## 5. Модель состояния ордера

### `OrderRecord`

Главный объект жизненного цикла ордера хранит:

- внутренний `order_id` и внешний `exchange_order_id`;
- `symbol`, `side`, `position_side`, `trade_side`;
- `order_type`, `tif`, цену и объёмы;
- `state`;
- `strategy_id`, `correlation_id`;
- timestamps создания и обновления;
- `rejection_reason`;
- `OrderExecutionInfo`.

### `OrderExecutionInfo`

Содержит:

- список `FillEvent`;
- `PartialFillPolicy`;
- `client_order_id`;
- `retry_count`;
- `time_in_open_ms` и `first_fill_latency_ms`;
- `expected_fill_price` и `realized_slippage`.

### Состояния FSM

Поддерживаются состояния:

```text
New
PendingAck
Open
PartiallyFilled
Filled
CancelPending
Cancelled
Rejected
Expired
UnknownRecovery
```

Ключевые разрешённые переходы:

```text
New -> PendingAck
PendingAck -> Open | Rejected | Filled | PartiallyFilled | CancelPending | Cancelled | UnknownRecovery
Open -> PartiallyFilled | Filled | CancelPending | Cancelled | Expired | UnknownRecovery
PartiallyFilled -> Filled | CancelPending | Cancelled | Expired | UnknownRecovery
CancelPending -> Cancelled | Filled | PartiallyFilled | UnknownRecovery
UnknownRecovery -> любое состояние
```

Терминальные состояния: `Filled`, `Cancelled`, `Rejected`, `Expired`.

Активные состояния: `PendingAck`, `Open`, `PartiallyFilled`, `CancelPending`.

## 6. `OrderRegistry`: единый источник правды

`OrderRegistry` выполняет сразу несколько задач.

### 6.1. Хранение ордеров и FSM

Реестр хранит:

- карту `orders_` по `order_id`;
- карту `fsms_` по `order_id`;
- набор `fill_applied_` для идемпотентности;
- карту `recent_intents_` для dedup окна.

### 6.2. FSM-синхронизация

При `transition(...)` состояние меняется сразу в двух местах:

1. внутри `OrderFSM`;
2. внутри соответствующего `OrderRecord`.

Это и есть основа всей state-модели execution-модуля.

### 6.3. Идемпотентность fills

`fill_applied_` нужен, чтобы один и тот же fill не был дважды применён к портфелю.

### 6.4. Дедупликация intent-ов

`recent_intents_` хранит уже отправленные торговые интенты и чистится по TTL-окну.

### 6.5. Очистка старых terminal orders

Есть отдельная очистка терминальных ордеров по возрасту, чтобы не раздувать память рантайма.

## 7. `ExecutionPlanner`: что он умеет и как принимает решение

Planner в текущей реализации достаточно богатый.

### 7.1. Классификация action

Он различает:

- `OpenPosition`;
- `ClosePosition`;
- `ReducePosition`;
- `EmergencyFlattenSymbol`;
- `NoAction`.

Основной источник истины здесь — `signal_intent`, а fallback идёт через `trade_side`.

### 7.2. Выбор execution style

На стиль влияют:

- `ExecutionAlphaResult.recommended_style`;
- `urgency_score`;
- `market.spread_bps`;
- текущий action;
- stale-флаг market context.

Логика в общем виде такая:

1. выходы, emergency и reduction всегда тяготеют к агрессивному исполнению;
2. агрессивный сигнал execution alpha ведёт к `AggressiveMarket`;
3. post-only возможен только если включён в конфиге;
4. низкий urgency и узкий spread ведут к `PassiveLimit`;
5. промежуточный случай уходит в `SmartFallback`;
6. stale market data форсирует `AggressiveMarket`.

### 7.3. Преобразование style -> order_type / tif

Теоретически planner умеет разложить стили на разные типы ордера, но фактически внутри `style_to_order_type()` уже есть упрощение:

- `AggressiveMarket` -> `Market`;
- `PassiveLimit` -> `Market`;
- `SmartFallback` -> `Market`;
- `CancelIfNotFilled` -> `Market`;
- `ReduceOnly` -> `Market`;
- `PostOnlyLimit` -> `PostOnly`.

Дальше `ExecutionEngine::create_order_record()` и это тоже дополнительно перетирает до `Market + IOC`.

Итог: planner по смыслу моделирует более богатый execution policy, чем реально используется downstream.

### 7.4. Ценообразование

Для market-стиля planner старается взять reference price из одного из источников:

1. `exec_alpha.recommended_limit_price`;
2. `intent.snapshot_mid_price`;
3. mid из `best_bid/best_ask`.

Для limit-стиля он умеет рассчитывать улучшенную цену от mid с поправкой на side.

### 7.5. Explainability

Planner всегда складывает текстовые причины выбора в `plan.reasons`, например:

- action;
- spread;
- urgency;
- reduce-only;
- forced market из-за stale data.

## 8. Обработка fills

### 8.1. `process_market_fill(...)`

Это основной happy path текущего runtime.

Что делает метод:

1. проверяет idempotency guard;
2. достаёт ордер из registry;
3. переводит FSM в `Filled`;
4. записывает фактические `filled_quantity`, `avg_fill_price`, `exchange_order_id`;
5. помечает fill как применённый;
6. применяет результат к портфелю;
7. обновляет метрики и лог.

### 8.2. `process_fill_event(...)`

Этот путь нужен для частичных и внешних fills.

Он умеет:

- накапливать `filled_quantity`;
- пересчитывать среднюю цену;
- сохранять историю `FillEvent`;
- вычислять latency до первого fill;
- пересчитывать realized slippage;
- переводить ордер в `PartiallyFilled` или `Filled`.

Если cumulative fill дошёл до полного объёма, fill применяется к портфелю.

### 8.3. Применение к портфелю

`apply_fill_to_portfolio(...)` делит операции на два класса:

1. открывающий fill — создаёт/обновляет позицию через `open_position(...)`;
2. закрывающий fill — уменьшает позицию через `reduce_position(...)` и считает gross PnL.

PnL считается по простой формуле:

- для long: `(exit - entry) * qty`;
- для short: `(entry - exit) * qty`.

Комиссия учитывается через `kDefaultTakerFeePct` и записывается в портфель.

## 9. Отмена ордеров

`CancelManager` реализует четыре основных сценария:

1. отмена одного ордера;
2. отмена всех ордеров по символу;
3. глобальная отмена всех активных ордеров;
4. отмена по timeout.

Внутренний путь `do_cancel(...)` выглядит так:

```text
проверка существования ордера
-> FSM: CancelPending
-> submitter.cancel_order(...)
-> при успехе FSM: Cancelled
-> release reserved cash
```

Если биржа не подтвердила cancel, ордер остаётся в `CancelPending` и предполагается, что дальше его подхватит recovery.

## 10. Recovery и reconciliation

`RecoveryManager` предназначен для сценариев, где локальное состояние ордера перестало быть надёжным.

### 10.1. `mark_uncertain_orders()`

Все активные ордера в состояниях `PendingAck` или `Open` переводятся в `UnknownRecovery`.

### 10.2. `recover_unknown_orders()`

Для каждого ордера в `UnknownRecovery` вызывается:

```text
submitter.query_order_fill_price(exchange_order_id, symbol)
```

Интерпретация ответа очень простая:

- если цена > 0, ордер считается `Filled`;
- если цена == 0, ордер консервативно считается `Cancelled`.

Это fail-safe логика, но это именно proxy на основе fill price, а не полноценный запрос статуса ордера.

### 10.3. `run_reconciliation()`

Полный цикл состоит из:

1. mark uncertain;
2. recover unknown;
3. force-resolve старых `CancelPending`.

### 10.4. Что важно для практики

Хотя API `run_reconciliation()` есть в `ExecutionEngine`, в текущем `TradingPipeline` этот метод не вызывается напрямую. В pipeline есть отдельный runtime reconciliation через `reconciliation_engine_->reconcile_active_orders(active_orders)`.

То есть внутренний `RecoveryManager` существует и рабочий, но по текущим поискам по коду не является главным reconciliation-контуром runtime.

## 11. Телеметрия исполнения

`ExecutionMetrics` собирает:

- `total_orders`;
- `filled_orders`;
- `rejected_orders`;
- `cancelled_orders`;
- `timed_out_orders`;
- `recovery_events`;
- `avg_submit_to_fill_ms`;
- `avg_slippage_bps`.

Он также пишет counters/gauges в registry метрик.

Практически важные нюансы:

1. slippage реально считается и пишется;
2. `avg_submit_to_fill_ms` умеет считаться, но в обычном market path `record_fill(...)` вызывается с `latency_ms = 0`, поэтому runtime latency здесь пока не очень информативна;
3. поле `total_fees_usdt` есть в `ExecutionStats`, но по текущему коду не обновляется.

## 12. Smart TWAP

`SmartTwapExecutor` не является частью `ExecutionEngine` в узком смысле. Это отдельный исполнитель, которым управляет pipeline.

### 12.1. Когда включается TWAP

Pipeline активирует TWAP в двух случаях:

1. если этого хочет `ExecutionAlpha` через `slice_plan`;
2. если сам `SmartTwapExecutor::should_use_twap(...)` считает ордер слишком крупным относительно ликвидности.

### 12.2. Как строится план TWAP

При создании плана учитываются:

- approved quantity после risk engine;
- текущая ликвидность;
- mid price;
- минимальный допустимый notional на slice;
- participation rate;
- фронтлоадинг по ранним слайсам.

TWAP создаёт набор `TwapSlice`, у каждого есть:

- индекс;
- целевой объём;
- limit/reference цена;
- плановое время отправки;
- sent/filled-флаги.

### 12.3. Как pipeline исполняет TWAP

На каждом тике pipeline:

1. смотрит, есть ли активный TWAP;
2. запрашивает следующий slice через `get_next_slice(...)`;
3. прогоняет этот slice через `ExecutionAlpha` и `RiskEngine`;
4. вызывает `execution_engine_->execute(...)` для slice-intent;
5. при успехе помечает slice как заполненный.

### 12.4. Практическое ограничение TWAP

`get_next_slice(...)` конструирует новый `TradeIntent` не как полную копию исходного, а как минимальный объект. В него явно переносятся только часть полей: `symbol`, `side`, `suggested_quantity`, `signal_name`, `correlation_id`, `limit_price`, `conviction`.

Из-за этого ряд futures-специфичных полей остаётся на значениях по умолчанию структуры `TradeIntent`, например:

- `position_side` по умолчанию `Long`;
- `trade_side` по умолчанию `Open`;
- `signal_intent` по умолчанию `LongEntry`;
- `urgency` по умолчанию `0.0`.

Это значит, что TWAP-контур в текущем виде лучше рассматривать как рабочий каркас, а не как полностью нейтральный по отношению к long/short/close-сценариям механизм.

## 13. Как execution встроен в `TradingPipeline`

### Инициализация

При старте pipeline:

1. выбирается submitter;
2. в paper-режиме используется `PaperOrderSubmitter`;
3. в production futures-режиме подключается `BitgetFuturesOrderSubmitter`;
4. создаётся `ExecutionEngine`;
5. в него сразу передаётся default leverage;
6. отдельно создаётся `SmartTwapExecutor`.

### Обычный путь торгового сигнала

В рабочем цикле pipeline последовательность выглядит так:

```text
Strategy
-> ExecutionAlpha
-> RiskEngine
-> опционально SmartTwapExecutor
-> Leverage sync в ExecutionEngine
-> anti-fingerprinting noise/jitter
-> ExecutionEngine::execute(...)
```

### Watchdog

`OrderWatchdog` периодически:

1. получает `active_orders()`;
2. вызывает `cancel_timed_out_orders(...)`;
3. классифицирует слишком старые `PendingAck`, `Open`, `CancelPending`, `PartiallyFilled`, `UnknownRecovery`;
4. инициирует cancel или alert.

### Runtime reconciliation

Pipeline также отдельно запускает reconciliation через самостоятельный `reconciliation_engine_`, а не через `execution_engine_->run_reconciliation()`.

## 14. `PaperOrderSubmitter`

Для тестов и paper-сценариев в самом execution-модуле есть простая реализация submitter-а.

Она умеет:

- округлять quantity по `ExchangeSymbolRules`;
- валидировать минимальный размер quantity;
- валидировать минимальный notional;
- генерировать synthetic `exchange_order_id`;
- всегда успешно отвечать на cancel.

Это удобный тестовый слой, который даёт realistic exchange floor даже без реальной биржи.

## 15. Что покрыто тестами

На момент анализа в `tests/unit/execution/execution_test.cpp` находится 41 `TEST_CASE`.

Покрыты следующие зоны:

1. FSM и таблица переходов.
2. `PaperOrderSubmitter`.
3. генератор client order id.
4. `OrderRegistry`.
5. базовые сценарии planner-а.
6. `ExecutionMetrics`.
7. базовый happy path `ExecutionEngine`.
8. отказы по risk / execution alpha / uncertainty.
9. dedup по correlation id.
10. zero quantity.
11. cleanup terminal orders.
12. сценарий short-open с reserve/release маржи на reject.
13. строковые helper-методы execution types.

Что напрямую не видно покрытым отдельными unit-тестами:

1. `SmartTwapExecutor`.
2. `FillProcessor` как отдельная подсистема.
3. `CancelManager` как отдельная подсистема.
4. `RecoveryManager` как отдельная подсистема.
5. асинхронные order update / fill event сценарии из live exchange integration.

## 16. Главные практические выводы

### 16.1. Что в модуле уже хорошо сделано

1. Архитектура модульная и понятная: registry/planner/fills/cancel/recovery/metrics разделены аккуратно.
2. Есть FSM и явная модель состояния ордера.
3. Есть дедупликация интентов и идемпотентность fills.
4. Есть учёт reserve cash/margin и авто-уменьшение размера под доступный капитал.
5. Есть отдельный watchdog и отдельный reconciliation-контур в pipeline.
6. Есть готовая абстракция submitter-а, поэтому live/paper режимы хорошо отделены.

### 16.2. Какие ограничения видны прямо из текущего кода

1. Planner умеет limit/post-only/fallback, но реальный `ExecutionEngine` почти всё принудительно сводит к `Market + IOC`.
2. Публичные hooks `on_order_update(...)` и `on_fill_event(...)` есть, но по текущим поискам по проекту pipeline их напрямую не вызывает.
3. `run_reconciliation()` внутри execution существует, но runtime reconciliation в pipeline идёт через отдельный модуль `reconciliation_engine_`.
4. `PartialFillPolicy` хранится в ордере, но end-to-end поведение по политике partial fills пока не доведено до полноценного рабочего контура.
5. TWAP интегрирован через pipeline отдельно от `ExecutionEngine` и пока реконструирует slice intent не полностью.
6. Метрики исполнения полезны, но часть полей пока не доведена до полноценного runtime-наполнения.

## 17. Итог

Execution-модуль в текущем состоянии — это не просто «функция отправки ордера», а уже полноценный orchestration layer для исполнения: с отдельной state machine, registry, planner-ом, обработкой fills, recovery, watchdog-friendly API и telemetry.

При этом его реальный рабочий профиль на сегодня — быстрые рыночные ордера по futures, где богатая теоретическая модель исполнения уже заложена в архитектуру, но часть возможностей пока остаётся подготовленной инфраструктурой, а не полностью задействованным production-потоком.

Если смотреть прагматично, модуль уже пригоден как ядро execution-контура для текущего futures bot-а, но понимать его нужно в двух слоях одновременно:

1. как аккуратно разложенную execution-архитектуру;
2. как систему, где реальный runtime-path заметно уже и агрессивнее, чем декларируют типы и planner.