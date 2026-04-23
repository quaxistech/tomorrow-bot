# Strategy Module Analysis

## 1. Что реально является production-стратегией

На текущий момент в production path используется только одна стратегия:
- `tb::strategy::StrategyEngine`
- `strategy_id = "scalp_engine"`
- версия `v2`

Это подтверждается pipeline:
- в `trading_pipeline.cpp` в `StrategyRegistry` регистрируется только `StrategyEngine`;
- других стратегий в рантайме не регистрируется;
- allocator и decision engine дальше работают только с тем, что вернул `StrategyRegistry`.

Вывод: фактически в проекте уже соблюдена цель "должна быть только одна скальперская стратегия". В runtime действительно работает только `scalp_engine`.

Дополнительно был найден legacy-хвост:
- `src/strategy/microstructure_scalp/microstructure_scalp_strategy.cpp`

Этот файл не входил в `src/strategy/CMakeLists.txt`, не имел видимого `.hpp`, не был зарегистрирован в pipeline и не покрывался тестами. Он являлся orphan/legacy code и был удалён.

## 2. Архитектура strategy модуля

Модуль состоит из следующих активных частей:

- `strategy_engine.hpp/.cpp`
  Центральный orchestration layer. Реализует `IStrategy` и управляет полным жизненным циклом сигнала.

- `state/strategy_state.hpp/.cpp`
  Машина состояний одного инструмента.

- `state/setup_models.hpp`
  Модели `Setup`, `StrategyPositionContext`, `MarketContextResult`, `SetupValidationResult`, `PositionManagementResult`.

- `context/market_context.hpp/.cpp`
  Первичная оценка пригодности market context для скальпинга.

- `setups/setup_lifecycle.hpp/.cpp`
  Детектор сетапов и валидатор сетапов.

- `management/position_manager.hpp/.cpp`
  Сопровождение позиции и генерация exit/reduce решений.

- `strategy_registry.hpp/.cpp`
  Реестр стратегий.

## 3. Жизненный цикл сигнала

Полный production flow выглядит так:

1. Pipeline строит `StrategyContext`.
2. В context передаются:
- `FeatureSnapshot`
- regime label
- world state label
- uncertainty level
- uncertainty multipliers
- HTF trend context
- текущая позиция
- risk summary
- flags `data_fresh`, `exchange_ok`, `futures_enabled`

3. Pipeline вызывает `StrategyEngine::evaluate(ctx)`.
4. Внутри `evaluate()` происходят верхнеуровневые блокировки:
- emergency halt
- day lock / symbol lock
- stale data / exchange unavailable
- disabled strategy

5. Далее движок синхронизирует внутреннее состояние позиции со входным `StrategyContext`.
6. После этого логика маршрутизируется по state machine:
- `Cooldown` -> `handle_cooldown()`
- pre-entry states -> `handle_pre_entry()`
- position states -> `handle_position()`

## 4. Машина состояний

Используются 11 состояний:
- `Idle`
- `Candidate`
- `SetupForming`
- `SetupPendingConfirmation`
- `EntryReady`
- `EntrySent`
- `PositionOpen`
- `PositionManaging`
- `ExitPending`
- `Cooldown`
- `Blocked`

По факту production flow чаще всего идёт так:

`Idle -> SetupForming -> SetupPendingConfirmation -> EntrySent -> PositionOpen -> PositionManaging -> ExitPending -> Cooldown -> Idle`

Замечание:
- состояние `Candidate` формально есть, но в текущем `StrategyEngine` почти не используется как самостоятельная meaningful stage;
- `EntryReady` тоже объявлено, но фактическая логика быстро переводит setup из confirmation прямо в `EntrySent`.

То есть формально state machine богатая, но реально боевой путь компактнее, чем кажется по enum.

## 5. Какие сетапы реализованы

Внутри `StrategyEngine` есть 4 под-сценария:

1. `MomentumContinuation`
- Основной импульсный сценарий.
- Требует ADX, momentum, дисбаланс стакана, buy/sell ratio.
- BUY и SELL формируются по принципу "2 из 3 подтверждений".
- Есть RSI guard, Bollinger guard, EMA trend guard и HTF guard.

2. `Retest`
- Использует Bollinger bands как proxy уровня.
- Ищет возврат цены к середине диапазона после пробоя.
- Подтверждается `momentum_20` и `book_imbalance_5`.

3. `PullbackInMicrotrend`
- Требует более сильный тренд, чем momentum setup.
- Использует EMA20/EMA50, ADX, momentum_20, momentum_5.
- Ищет откат против микротренда с сохранением общей структуры.

4. `Rejection`
- Контртрендовый сценарий.
- Использует экстремумы `bb_percent_b`, разворотный `momentum_5` и подтверждение по `book_imbalance_5`.
- Confidence у него ниже, чем у остальных.

## 6. Market context: как стратегия решает, можно ли вообще торговать

Перед детекцией сетапа вызывается `MarketContextEvaluator`.

Он проверяет:
- свежесть данных;
- валидность микроструктуры;
- спред;
- ликвидность;
- VPIN toxicity;
- пригодность волатильности.

Результат сводится к одному из качеств:
- `Excellent`
- `Good`
- `Marginal`
- `Poor`
- `Invalid`

Если контекст `Invalid` или `Poor`, новые входы не допускаются.

Важно:
- это хороший production guard для скальпера;
- но фильтр пока довольно короткий и использует не весь доступный набор современных microstructure-признаков.

## 7. Как происходит подтверждение сетапа

После детекции setup не торгуется сразу.

Дальше есть этап валидации:
- проверка таймаута setup;
- ухудшение спреда;
- деградация ликвидности;
- VPIN toxicity;
- разворот imbalance против setup;
- уход цены слишком далеко от reference;
- пробой stop-level.

Отдельно `can_confirm()` проверяет:
- прошло ли `setup_confirmation_window_ms`;
- спред всё ещё нормальный;
- imbalance остался в нужную сторону.

Ключевой вывод:
- подтверждение сейчас в основном time-based;
- это допустимо как первая production-версия,
- но не уровень "ультрасовременной" futures scalp system, где подтверждение должно быть event-based и price-action-based, а не просто "подождали N миллисекунд".

## 8. Position management: как стратегия сопровождает открытую позицию

После открытия позиции работает `PositionManager`.

Все exit/reduce решения централизованы в `PositionExitOrchestrator` (pipeline).
`PositionManager::evaluate()` всегда возвращает Hold — стратегия НЕ имеет собственных exit-решений.

Виды выходов (orchestrator):
1. **Hard risk stop** — capital% или price% от entry
2. **Trailing stop** — Chandelier Exit с адаптивным ATR множителем
3. **Quick profit** — fee-aware, gated by continuation value
4. **Partial take-profit** — profit >= N×ATR, anti-dust protected
5. **Toxic flow exit** — VPIN + adverse pressure
6. **Structural failure** — EMA cross + momentum reversal
7. **Liquidity deterioration** — book thinning + spread widening
8. **Continuation value exit** — 10-component model (time = hazard, not hard trigger)
9. **Funding carry exit** — funding rate penalty

Каждый выход имеет ExitExplanation: primary_driver, secondary_drivers, counterfactual.

6. `Order book deterioration`
- сильный adverse imbalance, но только при крупном убытке.

Сознательно отключены внутри strategy:
- strategy trailing stop;
- momentum exhaustion exit.

Причина, указанная в коде:
- часть stop/trailing уже делает pipeline,
- двойное срабатывание снижает win rate.

Это архитектурно разумно, но означает, что часть логики распределена между strategy и pipeline, а не замкнута внутри strategy модуля.

## 9. Какие индикаторы и признаки реально реализованы в проекте

### 9.1. Реально реализованы в feature layer

В `FeatureSnapshot` и `FeatureEngine` присутствуют:

Базовые технические:
- SMA
- EMA fast/slow
- RSI
- MACD
- Bollinger Bands
- ATR
- ADX (+DI / -DI)
- OBV
- volatility_5 / volatility_20
- momentum_5 / momentum_20

Продвинутые technical / market structure:
- CUSUM
- Volume Profile (`vp_poc`, `vp_value_area_high`, `vp_value_area_low`, `vp_price_vs_poc`)
- Time-of-Day profile (`tod_*`)
- weighted mid price
- trade VWAP
- book imbalance 5/10
- buy/sell ratio
- aggressive flow
- liquidity ratio
- book instability
- VPIN

### 9.2. Что из этого реально использует активный `StrategyEngine`

Используется напрямую:
- `spread_bps`
- `spread`
- `mid_price`
- `book_imbalance_5`
- `buy_sell_ratio`
- `liquidity_ratio`
- `bid_depth_5_notional`
- `ask_depth_5_notional`
- `vpin_toxic`
- `vpin_valid`
- `ema_20`
- `ema_50`
- `ema_valid`
- `rsi_14`
- `rsi_valid`
- `adx`
- `adx_valid`
- `atr_14`
- `atr_valid`
- `bb_percent_b`
- `bb_valid`
- `momentum_5`
- `momentum_20`
- `momentum_valid`
- `volatility_5`
- `volatility_20`
- `volatility_valid`
- `execution_context.is_feed_fresh`

### 9.3. Реализовано в проекте, но active strategy сейчас НЕ использует

Не используются активной production-стратегией:
- `sma_20`
- `macd_line`
- `macd_signal`
- `macd_histogram`
- `obv`
- `obv_normalized`
- `weighted_mid_price`
- `trade_vwap`
- `aggressive_flow`
- `book_imbalance_10`
- `book_instability`
- `cusum_positive`
- `cusum_negative`
- `cusum_threshold`
- `cusum_regime_change`
- `vp_poc`
- `vp_value_area_high`
- `vp_value_area_low`
- `vp_price_vs_poc`
- `tod_volatility_mult`
- `tod_volume_mult`
- `tod_alpha_score`

## 10. Проверка: все ли нужные индикаторы реализованы для проекта

### 10.1. Ответ в строгом смысле

Если вопрос звучит так:
- "реализованы ли в проекте современные индикаторы и microstructure-признаки, которые вообще могут быть нужны для scalp futures system?"

Тогда ответ:
- да, значительная часть уже реализована на feature layer;
- проект заметно сильнее среднего по набору сигналов.

Уже есть:
- VPIN
- CUSUM
- Volume Profile
- Time-of-Day profile
- book imbalance
- liquidity ratio
- trade VWAP
- weighted mid
- book instability
- ADX / ATR / BB / EMA / RSI / MACD / OBV / momentum / volatility

### 10.2. Но если вопрос звучит так:
- "использует ли текущая production-стратегия всё необходимое для действительно современной сверхприбыльной скальперской системы?"

Тогда ответ:
- нет, не использует.

Главный разрыв не в отсутствии вычисления индикаторов, а в том, что активная стратегия читает только часть уже готового feature-space.

## 11. Что критично не хватает активной production-стратегии

### 11.1. Не хватает использования уже реализованных признаков

Сильнейший practical gap:
- `MACD`, `OBV`, `VWAP`, `weighted_mid_price`, `book_instability`, `CUSUM`, `Volume Profile`, `Time-of-Day` уже посчитаны,
- но active `scalp_engine` их почти полностью игнорирует.

Это значит:
- у стратегии есть богатый входной data-plane,
- но decision-plane пока использует только его упрощённую часть.

### 11.2. Нет CVD

В проекте не найдено реализации `CVD` / `Cumulative Volume Delta`.

Для современной futures scalp strategy это реальный пробел, потому что:
- CVD помогает разделить истинное continuation pressure и ложный book imbalance;
- это один из наиболее полезных short-horizon order-flow indicators для perpetual futures.

### 11.3. Confirmation model всё ещё слишком простая

Сейчас setup confirmation по сути опирается на:
- истечение `setup_confirmation_window_ms`;
- сохранение спреда;
- сохранение знака imbalance.

Для modern scalp этого мало.
Нужна event-based confirmation логика, например:
- удержание уровня;
- повторный агрессивный flow в сторону setup;
- сохранение delta pressure;
- отсутствие microstructure collapse в течение N обновлений стакана;
- подтверждение через VWAP / weighted mid / value area reaction.

### 11.4. Strategy почти не использует uncertainty context

В `StrategyContext` приходят:
- `uncertainty`
- `uncertainty_size_multiplier`
- `uncertainty_threshold_multiplier`

Но активная стратегия их по сути не использует в собственном logic layer.
Они живут upstream/downstream, но не модифицируют setup detection внутри strategy.

Это ослабляет интеграцию между uncertainty и actual entry logic.

### 11.5. Нет полноценной integration with volume profile

`Volume Profile` реализован, но strategy не использует:
- `vp_poc`
- `vp_value_area_high`
- `vp_value_area_low`
- `vp_price_vs_poc`

Для скальпинга это серьёзная потеря, потому что эти уровни хорошо подходят для:
- rejection setup;
- retest setup;
- mean-reversion scalp;
- trap filtering near low-liquidity tails.

### 11.6. Нет использования session alpha

`Time-of-Day` есть, но strategy его не читает.
Значит стратегия не адаптирует:
- порог входа;
- hold time;
- conviction;
- размер агрессии
под европейскую/американскую/азиатскую ликвидность.

Для USDT-M scalp это важный practical gap.

## 12. Есть ли spot-логика

Прямой spot-oriented торговой логики в active production strategy нет.

Что есть:
- во всех SELL setup’ах присутствует guard:
  - если `futures_enabled == false`, то без существующей позиции short-entry запрещён.

Это не spot strategy logic, а корректная защита совместимости:
- на futures можно открывать short;
- на spot SELL без позиции не допускается.

Вывод:
- active production path futures-native;
- spot-specific торговых сценариев внутри `scalp_engine` нет.

## 13. Текущее качество стратегии

### Сильные стороны
- реально production-ready orchestration;
- state machine есть и она аккуратная;
- есть separation of concerns;
- защита от stale data / emergency halt / locks / toxic flow;
- есть HTF trend guard;
- есть multi-setup architecture;
- есть VPIN-based exit;
- есть decent microstructure foundation;
- единственная стратегия в runtime уже соответствует текущей архитектурной цели.

### Слабые стороны
- `ScalpStrategyConfig` не интегрирован в `AppConfig` / YAML;
- часть enum/states избыточна относительно реального flow;
- setup confirmation слишком прост;
- большой массив уже реализованных feature-signals не используется;
- нет CVD;
- нет adaptation by session alpha;
- нет integration with volume profile;
- uncertainty context почти не участвует в entry logic;
- часть exit logic сознательно отключена, поэтому стратегия зависит от pipeline-поведения.

## 14. Вывод по требованию "должна быть только сверхсовременная суперприбыльная скальперская стратегия"

С инженерной точки зрения в коде сейчас уже есть:
- одна реальная production-стратегия;
- она futures-oriented;
- она не содержит отдельной spot-логики;
- базово она годится как foundation для scalp system.

Но честный технический вывод такой:
- текущий `scalp_engine` ещё нельзя назвать "сверхсовременной" финальной стратегией топ-уровня;
- причина не в том, что модуль плохой,
- а в том, что project уже вычисляет больше качественных сигналов, чем стратегия реально использует.

То есть bottleneck сейчас не в отсутствии инфраструктуры,
а в недоиспользовании уже построенного feature-space.

## 15. Что нужно сделать дальше, чтобы стратегия действительно стала modern futures scalp alpha

Приоритет 1:
- интегрировать `ScalpStrategyConfig` в `AppConfig`, `config_loader`, `config_validator`, `production.yaml`;
- убрать хардкод-дефолты из runtime.

Приоритет 2:
- встроить в `StrategyEngine` уже существующие признаки:
  - `trade_vwap`
  - `weighted_mid_price`
  - `book_instability`
  - `cusum_regime_change`
  - `vp_poc`, `vp_value_area_high`, `vp_value_area_low`, `vp_price_vs_poc`
  - `tod_alpha_score`

Приоритет 3:
- добавить `CVD` / cumulative volume delta.

Приоритет 4:
- заменить time-based setup confirmation на event-based confirmation.

Приоритет 5:
- сделать uncertainty-aware setup logic:
  - повышать confidence threshold при росте uncertainty;
  - запрещать определённые setup types в `High/Extreme uncertainty`;
  - адаптировать hold time и aggression.

Приоритет 6:
- расширить тесты:
  - validation failures;
  - all four setup types;
  - position manager branches;
  - integration tests with real snapshots;
  - tests for volume profile / CUSUM / VPIN gating.

## 16. Финальный итог

1. В runtime сейчас действительно работает только одна скальперская стратегия: `scalp_engine`.
2. Legacy orphan `microstructure_scalp_strategy.cpp` был удалён.
3. Нужные для проекта индикаторы в целом реализованы на feature layer очень хорошо.
4. Но active strategy использует только часть из них.
5. Поэтому модуль strategy сейчас сильный, но ещё не предельный по качеству.
6. Главный резерв роста — не изобретение новых модулей, а грамотное подключение уже существующих signal layers к `scalp_engine`.
