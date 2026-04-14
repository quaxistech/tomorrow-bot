# Tomorrow Bot — Полный План Исправлений и Улучшений
## Углубленный Анализ Кодовой Базы (Фьючерсная USDT-M торговля на Bitget)

---

## СОДЕРЖАНИЕ

1. [КРИТИЧЕСКИЕ ОШИБКИ (Блокеры сборки)](#1-критические-ошибки)
2. [ОШИБКИ ЛОГИКИ И РАСЧЕТОВ](#2-ошибки-логики-и-расчетов)
3. [LEVERAGE ENGINE — Улучшения для максимальной прибыли](#3-leverage-engine)
4. [НЕИСПОЛЬЗУЕМЫЙ / МЕРТВЫЙ КОД](#4-мертвый-код)
5. [ЗАГЛУШКИ И НЕПОЛНЫЕ РЕАЛИЗАЦИИ](#5-заглушки-и-неполные-реализации)
6. [КОНФИГУРАЦИЯ И ВАЛИДАЦИЯ](#6-конфигурация-и-валидация)
7. [АРХИТЕКТУРНЫЕ ПРОБЛЕМЫ](#7-архитектурные-проблемы)
8. [THREAD SAFETY](#8-thread-safety)
9. [EXCHANGE API / BITGET ИНТЕГРАЦИЯ](#9-exchange-api)
10. [ТЕСТИРОВАНИЕ](#10-тестирование)
11. [СТИЛЬ КОДА](#11-стиль-кода)
12. [НАУЧНАЯ ОБОСНОВАННОСТЬ КОЭФФИЦИЕНТОВ](#12-научная-обоснованность)

---

## 1. КРИТИЧЕСКИЕ ОШИБКИ (Блокеры сборки)

### 1.1 Отсутствие http_server.cpp / http_server.hpp
- **Файлы**: `src/app/CMakeLists.txt:6`, `src/app/main.cpp:17`
- **Проблема**: CMakeLists.txt включает `http_server.cpp`, main.cpp делает `#include "http_server.hpp"`, но файлы НЕ СУЩЕСТВУЮТ на диске
- **Влияние**: Сборка невозможна
- **Исправление**: Создать `src/app/http_server.hpp` и `src/app/http_server.cpp` с реализацией `tb::app::HttpEndpointServer` для Prometheus metrics endpoint, ИЛИ удалить ссылки и использовать встроенный Boost.Beast HTTP сервер

### 1.2 Удалённый execution_engine.cpp без обновления заголовка
- **Файл**: `src/execution/execution_engine.cpp` (deleted в git)
- **Проблема**: Файл удален, вся реализация теперь в `execution_engine_new.cpp`. Если заголовок ссылается на символы из старого файла — линковка упадёт
- **Исправление**: Проверить что `execution_engine_new.cpp` полностью покрывает все объявления из `execution_engine.hpp`

---

## 2. ОШИБКИ ЛОГИКИ И РАСЧЕТОВ

### 2.1 Regime Engine: rsi_trend_bias default конфликт (КРИТИЧЕСКАЯ)
- **Файл**: `src/config/config_loader.cpp:515-516`
- **Проблема**: Загрузчик конфига использует default `"50.0"`, но структура `TrendThresholds` в `regime_config.hpp:35` имеет default `55.0`. RSI=50 — нейтральный, НЕ бычий. С bias=50 рынок с RSI=50 квалифицируется как StrongUptrend при высоком ADX
- **Научное обоснование**: RSI=50 — точка равновесия. Для подтверждения тренда нужен bias минимум 55 (Wilder, 1978)
- **Исправление**: Изменить default в config_loader на `"55.0"`

### 2.2 Pipeline использует v1 uncertainty (3 args) вместо v2 (5 args)
- **Файл**: `src/pipeline/trading_pipeline.cpp:2275`
- **Проблема**: Вызывается 3-аргументная версия assess(), подставляя нулевые portfolio/ML снапшоты. 3 из 9 измерений (portfolio_uncertainty, ml_uncertainty, correlation_uncertainty = 25% веса) ВСЕГДА РАВНЫ НУЛЮ
- **Влияние**: Система недооценивает неопределённость на 25%, завышая размер позиций
- **Исправление**: Собрать `PortfolioSnapshot` и `MlSignalSnapshot` перед вызовом и использовать 5-аргументную версию

### 2.3 Uncertainty: transition_uncertainty — инверсная логика
- **Файл**: `src/uncertainty/uncertainty_engine.cpp:465-467`
- **Проблема**: `base = regime.last_transition->confidence` — высокая уверенность в переходе создаёт ВЫСОКУЮ transition_uncertainty. Логически: уверенный переход в новый режим должен СНИЖАТЬ неопределённость
- **Исправление**: Использовать `base = 1.0 - regime.last_transition->confidence` или `base = transition_recency_factor * (1.0 - confidence)`

### 2.4 Uncertainty: correlation_uncertainty — возможная инверсия
- **Файл**: `src/uncertainty/uncertainty_engine.cpp:457`
- **Проблема**: `base = 1.0 - ml_signals.correlation_risk_multiplier`. Если multiplier > 1.0 (усиленный риск), uncertainty < 0 → clamp к 0. Высокая корреляция создает НУЛЕВУЮ неопределённость вместо высокой
- **Исправление**: Использовать `base = std::clamp(ml_signals.correlation_risk_multiplier - 1.0, 0.0, 1.0)` или нормализовать multiplier в [0,1]

### 2.5 Liquidation price: одинаковая entry и current price
- **Файл**: `src/leverage/leverage_engine.cpp:91-111`
- **Проблема**: `is_liquidation_safe()` использует `entry_price` как `current_price`. До открытия позиции это корректно, но не учитывает slippage и volatility между решением и исполнением
- **Исправление**: Добавить worst-case buffer: `effective_price = entry_price * (1 ± estimated_slippage)`

### 2.6 Stop-loss price_loss_pct: неверная формула для short
- **Файл**: `src/pipeline/trading_pipeline.cpp:1785`
- **Проблема**: reason string всегда использует формулу long `(entry - price) / entry`, даже для short позиций
- **Исправление**: Добавить проверку `position_side` и использовать `(price - entry) / entry` для short

### 2.7 SpotSellCheck и futures — `has_position` для short
- **Файл**: `src/pipeline/trading_pipeline.cpp:2630`
- **Проблема**: `has_open_long = portfolio_->has_position(symbol_)` проверяет ЛЮБУЮ позицию, не конкретно long. Для futures с short позицией flag будет true, но имя переменной и логика подразумевают long
- **Исправление**: Разделить на `has_long_position` и `has_short_position` с учётом `PositionSide`

### 2.8 is_idle() trick — не работает как задумано
- **Файл**: `src/pipeline/trading_pipeline.cpp:2594` и `2003`
- **Проблема**: Установка `last_activity_ns_ = 0` для принудительной ротации не работает — `is_idle()` возвращает false при `last == 0` (считая что тиков ещё не было)
- **Исправление**: Использовать `last_activity_ns_ = 1` или добавить специальный флаг `force_rotation_`

### 2.9 FillProcessor PnL — fragile для futures short
- **Файл**: `src/execution/fills/fill_processor.cpp:213-228`
- **Проблема**: Использует `Side::Buy/Sell` вместо `PositionSide::Long/Short` для определения long/short P&L. Работает случайно потому что Position.side совпадает.
- **Исправление**: Использовать `PositionSide` явно

### 2.10 Hold signal intent → OpenPosition в ExecutionPlanner
- **Файл**: `src/execution/planner/execution_planner.cpp:125`
- **Проблема**: `Hold` maps to `OpenPosition`. Если Hold intent пройдёт до execution engine, создастся ошибочный ордер.
- **Исправление**: Вернуть `ExecutionAction::NoAction` для `Hold`

### 2.11 Win rate / win-loss ratio hardcoded
- **Файл**: `src/pipeline/trading_pipeline.cpp:3049-3050`
- **Проблема**: `win_rate = 0.5`, `win_loss_ratio = 1.5` — никогда не обновляются из реальной истории торгов. Kelly fraction расчет в portfolio_allocator использует фиктивные данные
- **Исправление**: Вести скользящую статистику win_rate и avg_win/avg_loss из portfolio close events

---

## 3. LEVERAGE ENGINE — Улучшения для Оптимальной Прибыли

### 3.1 Kelly Criterion Integration
- **Проблема**: Текущий leverage engine не использует Kelly criterion для определения оптимального плеча
- **Научное обоснование**: Kelly criterion (Kelly, 1956) определяет оптимальную фракцию f* = (p·b - q) / b, где p=win rate, b=payoff ratio, q=1-p. Optimal leverage = f* / (σ² risk), где σ — волатильность (Thorp, 2006)
- **Исправление**: Добавить `kelly_optimal_leverage()` метод:
  ```
  half_kelly = 0.5 * (win_rate * avg_win - (1 - win_rate) * avg_loss) / (avg_loss * variance)
  ```
  Использовать half-Kelly (50% оптимума) для защиты от ошибок оценки

### 3.2 Drawdown multiplier — слишком агрессивный после 20%
- **Проблема**: С scale=0.5 при 20% drawdown множитель = 0.25. Floor 0.10 при 33%. Между 20-33% торговля продолжается с non-trivial leverage
- **Научное обоснование**: Ralph Vince "Leverage Space Trading Model" рекомендует полную остановку при drawdown > Target/2. При max_drawdown=10%, остановка должна быть при 5%
- **Исправление**: Использовать сигмоидную функцию вместо экспоненциальной:
  ```
  multiplier = 1.0 / (1.0 + exp(k * (drawdown - midpoint)))
  ```
  где midpoint = drawdown_stop / 2, k определяет крутизну

### 3.3 Conviction multiplier boost до 1.30 — недостаточно обоснован
- **Проблема**: Conviction=1.0 увеличивает leverage на 30%. В production с base=12 это 15.6x. Нет gradual increase — скачок от 1.0 до 1.3 резкий
- **Научное обоснование**: Гипотеза эффективного рынка (Fama, 1970) предполагает, что высокая conviction может быть ложной. Максимальный boost должен зависеть от калибровки conviction (Brier score)
- **Исправление**:
  - Ограничить boost до 1.15 (максимум +15%) без калибровки
  - Добавить conditional boost: conviction > 0.9 AND bayesian_calibration_score > 0.7 → bonus до 1.25
  - Логировать Brier score для ongoing calibration

### 3.4 Volatility multiplier — breakpoints не научно обоснованы
- **Проблема**: Breakpoints (0.5%, 2%, 4%, 8%, 12%) выбраны произвольно для "типичного альткоина"
- **Научное обоснование**: Nemenman et al. (2002) показывают что vol regimes лучше определяются через percentiles истории, а не фиксированные breakpoints. Parkinson (1980) volatility estimator даёт более стабильные оценки чем ATR
- **Исправление**:
  - Использовать rolling percentiles (20th, 50th, 80th, 95th) ATR за последние 100 периодов
  - Добавить адаптивные breakpoints на основе исторической волатильности символа

### 3.5 Funding rate multiplier — линейная модель неадекватна
- **Проблема**: Линейная формула `1 - penalty * excess` даёт отрицательные значения при high funding (clamped to 0.25). Funding rates > 0.09 все дают одинаковый множитель 0.25
- **Научное обоснование**: Funding rates следуют mean-reverting процессу (Ornstein-Uhlenbeck). Extreme funding предсказывает разворот позиции (Shintani & Linton, 2004)
- **Исправление**:
  ```
  multiplier = max(0.15, exp(-penalty * (|rate| - threshold) / threshold))
  ```
  Экспоненциальное затухание вместо линейного — smooth, всегда положительный

### 3.6 Отсутствие адаптации через Reinforcement Learning
- **Проблема**: Все множители — статические функции. Нет обратной связи от результатов торгов
- **Исправление**: Добавить EMA-feedback loop:
  ```
  leverage_performance[regime] = 0.95 * prev + 0.05 * (trade_pnl > 0 ? 1.1 : 0.9)
  ```
  Множитель регулирует base_leverage на основе результатов в каждом режиме

### 3.7 Integer leverage discretization — потеря точности
- **Проблема**: `std::round()` на float → int. 1.49 → 1x, 1.51 → 2x — 50% скачок для low leverage
- **Исправление**: Бирже отправлять int (требование API), но внутренний расчёт margin/risk вести на double. Добавить intermediate_leverage (double) в LeverageDecision

### 3.8 Cross-margin mode — формула ликвидации неверна
- **Файл**: `src/leverage/leverage_engine.cpp:135-155`
- **Проблема**: Всегда используется isolated margin formula. При `margin_mode = "crossed"` ликвидационная цена должна учитывать весь баланс аккаунта
- **Исправление**: Добавить ветку для cross margin:
  ```
  cross_liq_price = entry * (1 - (account_balance - used_margin) / (position_size * entry))
  ```

---

## 4. МЕРТВЫЙ КОД (Удалить)

### 4.1 tb_resilience — собирается, но НИКЕМ не линкуется
- **Файлы**: `src/resilience/` (circuit_breaker, retry_executor, idempotency_manager)
- **Проблема**: Ни один production модуль не линкует `tb_resilience`
- **Исправление**: Либо интегрировать в `tb_execution` и `tb_exchange_bitget`, либо удалить

### 4.2 PipelineTickContext — объявлен, но не используется
- **Файл**: `src/pipeline/pipeline_tick_context.hpp`
- **Проблема**: Полноценная структура для staged pipeline — НЕ используется в `on_feature_snapshot()`. `staged_tick_count_` никогда не инкрементируется
- **Исправление**: Либо рефакторить `on_feature_snapshot()` на staged pipeline с использованием этого контекста, либо удалить

### 4.3 PipelineStageResult — объявлен, но не используется
- **Файл**: `src/pipeline/pipeline_stage_result.hpp`
- **Проблема**: `StageOutcome` enum и factory functions никогда не вызываются
- **Исправление**: Аналогично 4.2

### 4.4 IntentExecution tracking struct — не используется
- **Файл**: `src/execution/execution_types.hpp:100-127`
- **Проблема**: Struct с detailed tracking никогда не инстанцируется
- **Исправление**: Удалить или интегрировать

### 4.5 SystemHealthState — не используется
- **Файл**: `src/execution/execution_types.hpp:152-158`
- **Исправление**: Удалить или интегрировать

### 4.6 PositionSizer — создаётся, но никогда не вызывается
- **Файл**: `src/risk/risk_engine.cpp`
- **Проблема**: `sizer_` member создается в конструкторе, но метод `compute_adjustment()` никогда не вызывается в `evaluate()`. Dead code
- **Исправление**: Интегрировать в policy chain или удалить

### 4.7 check_quote_freshness() — объявлен, но дублируется inline
- **Файл**: `src/pipeline/trading_pipeline.cpp:3408-3416`
- **Проблема**: Метод существует, но freshness check на строке 2021-2036 дублирует логику inline
- **Исправление**: Использовать метод вместо inline дублирования

### 4.8 WorldModel min_confidence_to_switch — загружается, но не используется
- **Файл**: `src/world_model/world_model_engine.cpp` (apply_hysteresis)
- **Проблема**: Конфиг `min_confidence_to_switch{0.55}` загружается из YAML, но гистерезис его не проверяет
- **Исправление**: Добавить проверку confidence в hysteresis или удалить параметр

### 4.9 WorldModel RegimeAdaptationConfig — заявлена, но не реализована
- **Файл**: `src/world_model/world_model_config.hpp:217`
- **Проблема**: Структура с per-regime ADX offsets, RSI tolerance, spread tolerance — никогда не читается кодом
- **Исправление**: Реализовать или удалить

### 4.10 Regime inertia_alpha — загружается, но не используется
- **Файл**: config_loader.cpp:563-564 и regime_engine.cpp
- **Проблема**: EMA smoothing parameter для transition inertia — dead config
- **Исправление**: Реализовать EMA smoothing в hysteresis или удалить

### 4.11 Execution: все order types кроме Market — dead code
- **Файлы**: `execution_engine_new.cpp:522-523`, `execution_planner.cpp:187-199`
- **Проблема**: Order type ВСЕГДА форсируется в Market (комментарий: "пока нет приватного WS"). Вся Limit/PostOnly/SmartFallback инфраструктура мертва
- **Примечание**: Не удалять, а пометить TODO — нужно для Phase 2 с приватным WS

### 4.12 scenario_integration тесты — никогда не компилируются
- **Файл**: `tests/CMakeLists.txt` — нет `add_subdirectory(unit/scenario_integration)`
- **Исправление**: Добавить строку или удалить тесты

---

## 5. ЗАГЛУШКИ И НЕПОЛНЫЕ РЕАЛИЗАЦИИ

### 5.1 OpportunityCost Upgrade action — stub
- **Файл**: `src/pipeline/trading_pipeline.cpp:3027-3033`
- **Проблема**: Комментарий "will be implemented in next phases", execute как fallthrough
- **Исправление**: Реализовать закрытие худшей позиции и открытие лучшей

### 5.2 reduce_only flag — не передаётся на биржу
- **Файлы**: `bitget_futures_order_submitter.cpp`
- **Проблема**: `ExecutionPlan.reduce_only` вычисляется планировщиком, но НЕ отправляется в JSON Bitget API. Bitget поддерживает `reduceOnly` параметр
- **Влияние**: Закрывающий ордер может случайно увеличить позицию при ошибке в логике direction
- **Исправление**: Добавить `{"reduceOnly", "true"}` в JSON для orders с `trade_side == Close`

### 5.3 Stop orders — не реализованы
- **Файлы**: `bitget_order_submitter.cpp`, `bitget_futures_order_submitter.cpp`
- **Проблема**: StopMarket и StopLimit явно rejected с error log. Нужен endpoint `/api/v2/mix/order/place-plan-order`
- **Исправление**: Реализовать plan-order endpoint для Stop Loss ордеров на бирже (server-side stops)

### 5.4 WebSocket — только public, нет private channel
- **Файл**: `src/exchange/bitget/bitget_ws_client.cpp`
- **Проблема**: URL hardcoded `/v2/ws/public`, нет authentication. Нет real-time fill notifications, balance updates, position changes
- **Влияние**: Вся информация о fills/balances/positions идёт через REST polling — latency 1-5 секунд
- **Исправление**: Добавить `/v2/ws/private` с HMAC-SHA256 auth, подписку на `orders`, `account`, `positions` channels

### 5.5 YAML секции adversarial_defense, ai_advisory, shadow — не загружаются
- **Файлы**: Все 4 YAML config файла содержат эти секции, но в `config_types.hpp` нет соответствующих структур
- **Исправление**: Добавить struct'ы и загрузку, или удалить из YAML

### 5.6 ExtendedRiskConfig — 28+ полей НЕ конфигурируемы
- **Файл**: `src/risk/risk_types.hpp:154-237`
- **Проблема**: Большинство параметров risk engine (max_positions, max_slippage_bps, cooldowns, strategy limits) используют hardcoded defaults и НЕ загружаются из YAML
- **Исправление**: Расширить YAML risk секцию и config_loader для загрузки всех полей

---

## 6. КОНФИГУРАЦИЯ И ВАЛИДАЦИЯ

### 6.1 initial_capital — no try/catch
- **Файл**: `src/config/config_loader.cpp:176`
- **Проблема**: `std::stod()` без обработки исключений. Non-numeric value → unhandled exception
- **Исправление**: Использовать `parse_double()` helper как для всех остальных числовых полей

### 6.2 min_leverage не валидируется против default_leverage
- **Файл**: `src/config/config_validator.cpp:368-383`
- **Проблема**: Нет проверки `min_leverage <= default_leverage`. min=10, default=5 пройдёт валидацию
- **Исправление**: Добавить `if (f.min_leverage > f.default_leverage) error`

### 6.3 Нет валидации pair_selection
- **Файл**: `src/config/config_validator.cpp`
- **Проблема**: Нет `validate_pair_selection()`. top_n, min_volume_usdt, rotation_interval — без проверки
- **Исправление**: Добавить валидатор

### 6.4 Нет валидации ScorerConfig weights sum
- **Файл**: `src/config/config_types.hpp:84-131`
- **Проблема**: `momentum_max + trend_max + tradability_max + quality_max` должны = 100, но не проверяется
- **Исправление**: Добавить cross-validation

### 6.5 price_stop_loss_pct, min_risk_reward_ratio, min_hold_minutes — без валидации
- **Файл**: `src/config/config_validator.cpp:197-232`
- **Исправление**: Добавить range checks

### 6.6 production.yaml max_leverage: 50 — слишком высоко
- **Файл**: `configs/production.yaml`
- **Проблема**: При 50x liquidation buffer = ~1.5% с mmr=0.004. Validator только warning, не error
- **Рекомендация**: Ограничить max_leverage до 20-25x в production, или сделать validator error для >25

### 6.7 paper.yaml metrics.port: 9090 = production порт
- **Файл**: `configs/paper.yaml`
- **Проблема**: Конфликт портов если оба режима на одной машине
- **Исправление**: Изменить paper port на 9093

### 6.8 shadow.yaml и testnet.yaml — missing initial_capital
- **Файлы**: `configs/shadow.yaml`, `configs/testnet.yaml`
- **Проблема**: Defaults to 10000 вместо production-like 4.58
- **Исправление**: Добавить explicit initial_capital

### 6.9 Silently ignore unknown YAML keys
- **Файл**: `src/config/config_loader.cpp`
- **Проблема**: Опечатка в ключе (напр. `futurs.enabled`) молча использует default
- **Исправление**: Добавить warning для ключей в flat map, не совпавших ни с одним known key

### 6.10 Нет config для spot-only mode
- **Проблема**: Все 4 YAML файла имеют `futures.enabled: true`. Path `futures.enabled: false` не тестируется
- **Исправление**: Создать `configs/spot-paper.yaml` с `futures.enabled: false`

---

## 7. АРХИТЕКТУРНЫЕ ПРОБЛЕМЫ

### 7.1 on_feature_snapshot() — 1300+ строк монолитная функция
- **Файл**: `src/pipeline/trading_pipeline.cpp:2010-3331`
- **Проблема**: Нарушение Single Responsibility. Невозможно тестировать отдельные стадии
- **Исправление**: Декомпозировать на staged pipeline используя уже готовые `PipelineTickContext` и `PipelineStageResult`:
  - `stage_preprocess(ctx)` — freshness, watchdog, stop-loss
  - `stage_analyze(ctx)` — world model, regime, uncertainty
  - `stage_evaluate(ctx)` — strategies, decision, Thompson sampling
  - `stage_filter(ctx)` — HTF, cooldowns, position classification
  - `stage_size_and_risk(ctx)` — opportunity cost, allocator, risk engine
  - `stage_execute(ctx)` — leverage, order submission

### 7.2 Множественные portfolio_->snapshot() за тик (7+)
- **Файл**: `src/pipeline/trading_pipeline.cpp`
- **Проблема**: 7+ вызовов snapshot() за один тик. Если snapshot() делает mutex lock + deep copy, это расточительно
- **Исправление**: Один вызов portfolio_->snapshot() в начале тика, использовать cached snapshot для всех стадий

### 7.3 REST API в critical path
- **Файлы**: `trading_pipeline.cpp` (balance sync, position query, leverage set)
- **Проблема**: Синхронные REST вызовы блокируют pipeline на 100-200ms каждый
- **Исправление**: Сделать balance sync и position queries async (separate thread), только leverage set оставить sync (критичный для безопасности)

### 7.4 REST client — нет connection pooling
- **Файл**: `src/exchange/bitget/bitget_rest_client.cpp`
- **Проблема**: Каждый запрос создаёт новое SSL соединение (DNS resolve + TCP connect + SSL handshake)
- **Исправление**: Реализовать connection pool с keep-alive

### 7.5 Circular dependencies (tb_exchange_bitget ↔ tb_execution ↔ tb_reconciliation)
- **Файлы**: CMakeLists.txt модулей
- **Проблема**: Нарушает clean layering, затрудняет компиляцию и тестирование
- **Исправление**: Ввести interface-only модуль `tb_exchange_interfaces` с абстрактными интерфейсами. Concrete implementations зависят от interfaces, а не друг от друга

### 7.6 tb_normalizer → tb_exchange_bitget dependency
- **Проблема**: Normalizer нужен только `bitget_models.hpp` (POD types), но линкует всю библиотеку
- **Исправление**: Выделить `bitget_models.hpp` в отдельный interface target

### 7.7 Unconditional PostgreSQL dependency
- **Файл**: `src/persistence/CMakeLists.txt`
- **Проблема**: `target_link_libraries(tb_persistence ... pqxx pq)` без `find_package(pqxx QUIET)`
- **Исправление**: Добавить `find_package(pqxx QUIET)` + `#ifdef HAS_PQXX` в коде

### 7.8 Adversarial severity — вычисляется в pipeline, а не в leverage engine
- **Файл**: `src/pipeline/trading_pipeline.cpp:3111-3182`
- **Проблема**: 70-строчный lambda для вычисления adversarial severity тесно связан с pipeline. Дублирование если нужен другой consumer
- **Исправление**: Перенести в метод `LeverageEngine::compute_adversarial_severity(WorldModelSnapshot, MlSnapshot)` или отдельный utility

---

## 8. THREAD SAFETY

### 8.1 ExecutionEngine::execute() — нет top-level mutex
- **Файл**: `src/execution/execution_engine_new.cpp`
- **Проблема**: Dedup check → plan → register → submit — sequential операции без overarching lock. Race condition при concurrent calls
- **Исправление**: Добавить mutex на execute()

### 8.2 Supervisor segfault при null logger
- **Файл**: `src/supervisor/supervisor.cpp:156-166`
- **Проблема**: `validate_startup_dependencies()` dereferences logger_ ПОСЛЕ проверки на null
- **Исправление**: Исправить логику: `if (!logger_) return Error; logger_->...`

### 8.3 Hot-swap race condition в idle monitor
- **Файл**: `src/app/main.cpp:474-615`
- **Проблема**: Background thread мутирует `active_symbols`, `pipelines` без mutex, в то время как shutdown handler может обращаться к тем же объектам
- **Исправление**: Добавить shared mutex для `pipelines` и `active_symbols`

### 8.4 Supervisor: register_open_position emplace silently fails
- **Файл**: `src/supervisor/supervisor.cpp:339`
- **Проблема**: `emplace` на duplicate symbol не обновляет данные → неверный position count
- **Исправление**: Использовать `insert_or_assign`

### 8.5 Circuit breaker time units mismatch
- **Файл**: `src/resilience/circuit_breaker.cpp`
- **Проблема**: `last_failure_time_ms` хранит наносекунды, сравнивается с millisecond timeout
- **Исправление**: Единые единицы измерения (наносекунды)

### 8.6 Supervisor wait_for_shutdown — spin polling
- **Файл**: `src/supervisor/supervisor.cpp`
- **Проблема**: `sleep_for(100ms)` в цикле
- **Исправление**: Использовать `condition_variable`

### 8.7 Uncertainty feedback_buffer — unbounded growth
- **Файл**: `src/uncertainty/uncertainty_engine.cpp:259`
- **Проблема**: `push_back(feedback)` без size limit. При длительной работе → memory leak
- **Исправление**: Circular buffer с max size (e.g., 10000 entries)

---

## 9. EXCHANGE API / BITGET ИНТЕГРАЦИЯ

### 9.1 Futures parse_order() — не извлекает tradeSide/holdSide
- **Файл**: `src/exchange/bitget/bitget_futures_query_adapter.cpp`
- **Проблема**: `ExchangeOrderInfo` не содержит `position_side` / `trade_side`. Reconciliation не может различить opening/closing или long/short
- **Исправление**: Расширить `ExchangeOrderInfo` struct, парсить из API response

### 9.2 REST client — нет retry logic
- **Файл**: `src/exchange/bitget/bitget_rest_client.cpp`
- **Проблема**: Network failures возвращаются как error без retry
- **Исправление**: Добавить retry с exponential backoff (max 3 attemps) для transient errors (timeout, DNS failure, 5xx)

### 9.3 Rate limiter — potential token over-consumption
- **Файл**: `src/exchange/bitget/bitget_rest_client.cpp:73-110`
- **Проблема**: Race condition между sleep и token consumption
- **Исправление**: После sleep пересчитать tokens и ждать заново если tokens < 1.0

### 9.4 set_leverage() halving — пропускает valid intermediate values
- **Файл**: `src/exchange/bitget/bitget_futures_order_submitter.cpp`
- **Проблема**: 20→10→5→2→1. Max=15 → sets to 10 вместо 15
- **Исправление**: Binary search: при ошибке 40797 делать `(current + 1) / 2` с `high = current - 1`

### 9.5 Scanner — нет API rate limiting
- **Файл**: `src/scanner/scanner_engine.cpp`
- **Проблема**: Multiple REST calls без rate limiting. Может hit Bitget rate limits
- **Исправление**: Использовать тот же `BitgetRestClient` с rate limiter, или добавить delays

### 9.6 Spot market buy — size в quote currency
- **Файл**: `src/exchange/bitget/bitget_order_submitter.cpp`
- **Проблема**: Для market buy size указывается в USDT (qty * price). Если price неточный, реальный fill amount отличается
- **Исправление**: После fill пересчитать actual quantity и обновить portfolio

---

## 10. ТЕСТИРОВАНИЕ (Приоритетные добавления)

### 10.1 CRITICAL — Pipeline тесты (0 тестов для ядра системы)
### 10.2 CRITICAL — Exchange adapter тесты (0 тестов)
### 10.3 HIGH — Futures end-to-end тест (short entry → leverage → execution → tracking)
### 10.4 HIGH — Config loader YAML десериализация (0 тестов)
### 10.5 HIGH — Scanner модуль тесты (0 тестов)
### 10.6 MEDIUM — Buffer модуль тесты (ring_buffer, candle_buffer)
### 10.7 MEDIUM — Common module тесты (strong types, enums)
### 10.8 MEDIUM — Feature Engine computation тесты (не только data structures)
### 10.9 LOW — Concurrency / thread safety тесты
### 10.10 LOW — Performance benchmarks
### 10.11 FIX — Добавить `scenario_integration` в tests/CMakeLists.txt

---

## 11. СТИЛЬ КОДА

### 11.1 Mixed languages — русский/английский в комментариях
- **Проблема**: Классы и секции на русском, inline код и логи на английском. Inconsistency
- **Рекомендация**: Выбрать один язык. Для production code — английский (международный стандарт). Русские комментарии конвертировать

### 11.2 Magic numbers в pipeline
- **Файл**: `src/pipeline/trading_pipeline.cpp`
- **Примеры**: `-0.1` (fingerprint edge), `92.0`/`8.0` (RSI extreme), `0.5` (exit conviction), `0.00001` (min quantity)
- **Исправление**: Вынести в named constants или конфиг

### 11.3 Repeated `tick_count_ % N == 0` pattern (20+ мест)
- **Исправление**: Utility `should_log_at_interval(N)`

### 11.4 WorldModel classify_immediate — dead assignment `total = 7`
- **Файл**: `src/world_model/world_model_engine.cpp:432-433`
- **Исправление**: Удалить `int total = 7;`

### 11.5 Top drivers sorting by raw magnitude across different scales
- **Файл**: `src/world_model/world_model_engine.cpp:1157`
- **Проблема**: ADX (0-100) и Vol5 (0-0.05) несопоставимы
- **Исправление**: Z-score normalize все driver values перед сортировкой

---

## 12. НАУЧНАЯ ОБОСНОВАННОСТЬ КОЭФФИЦИЕНТОВ

### 12.1 Leverage Volatility Breakpoints
- **Текущие**: 0.5%, 2%, 4%, 8%, 12% ATR/price
- **Рекомендация**: Адаптивные percentiles из исторических данных (Garman-Klass volatility estimator)

### 12.2 Drawdown Decay: `0.5^(dd/10)`
- **Научная основа**: Мультипликативный процесс (geometric random walk). Оптимальный decay rate связан с Edge/σ² (Optimal f, Vince 1992)
- **Рекомендация**: Параметризовать midpoint и steepness через backtesting

### 12.3 Conviction boost 1.30
- **Научная основа**: Отсутствует прямая. Kelly criterion допускает до 2x при идеальной калибровке
- **Рекомендация**: Связать с Brier score: boost = 1 + 0.3 * calibration_quality

### 12.4 Funding rate threshold 0.03 (3%)
- **Научная основа**: Средний funding rate BTC ~0.01%. 3% — экстремально высокий (3-4σ event)
- **Рекомендация**: Адаптивный threshold = rolling_mean + 2 * rolling_std

### 12.5 Maintenance margin rate 0.004 (0.4%)
- **Научная основа**: Bitget USDT-M maintenance margin rates vary by tier (от 0.4% до 5%)
- **Рекомендация**: Загружать из API для конкретного символа и tier

### 12.6 Hysteresis min_dwell_ticks: 3-5
- **Научная основа**: Regime persistence literature (Hamilton, 1989 — Markov switching models)
- **Рекомендация**: Связать dwell_ticks с estimated regime duration из transition matrix

### 12.7 Uncertainty EMA alpha: 0.15
- **Научная основа**: Для 1-second ticks, 0.15 даёт half-life ~4.3 ticks. Для 1-minute ticks — 4.3 минуты
- **Рекомендация**: Привязать к timeframe: `alpha = 2 / (desired_half_life_in_ticks + 1)`

### 12.8 Adversarial severity weights
- **Текущие**: spread(0.15), book_instability(0.20), vpin(0.10), aggressive_flow(0.15), fingerprint(-0.10)
- **Научная основа**: Weights не калибрированы. VPIN (Easley, López de Prado, O'Hara, 2012) — более предиктивный чем spread для toxicity
- **Рекомендация**: Калибровать через logistic regression на исторических данных adverse selection events

### 12.9 Thompson Sampling rewards binary (+1/-1)
- **Текущие**: `reward = won ? 1.0 : -1.0` (Beta-Bernoulli model)
- **Научная основа**: Потеря информации о magnitude. Gaussian Thompson Sampling (Agrawal & Goyal, 2013) лучше для continuous rewards
- **Рекомендация**: Перейти на Normal-TS: `reward = trade_pnl_bps` вместо binary

### 12.10 Kelly fraction with hardcoded win_rate=0.5, ratio=1.5
- **Текущие**: `f* = 0.5 * 1.5 - 0.5 = 0.25` (25% capital) — не зависит от реальных результатов
- **Научная основа**: Kelly (1956) требует АКТУАЛЬНЫЕ оценки вероятностей
- **Рекомендация**: Вести rolling window (последние 100 трейдов) win_rate и avg_win/avg_loss

---

## ПОРЯДОК ВЫПОЛНЕНИЯ (по приоритету)

### Фаза 1: Блокеры и критические ошибки (P0)
1. [1.1] Создать http_server.hpp/cpp
2. [2.1] Исправить rsi_trend_bias default
3. [2.2] Переключить pipeline на v2 uncertainty
4. [5.2] Добавить reduce_only в futures orders
5. [6.1] Защитить initial_capital parsing
6. [8.2] Исправить supervisor segfault

### Фаза 2: Логика торговли (P1)
7. [2.3] Исправить transition_uncertainty инверсию
8. [2.4] Исправить correlation_uncertainty инверсию
9. [2.6] Исправить stop-loss reason для short
10. [2.7] Разделить has_position на long/short
11. [2.8] Исправить is_idle() trick
12. [2.10] Hold → NoAction в planner
13. [2.11] Реализовать rolling win_rate/ratio
14. [9.1] Расширить ExchangeOrderInfo для futures

### Фаза 3: Leverage Engine оптимизация (P1)
15. [3.1] Добавить Kelly-based optimal leverage
16. [3.2] Сигмоидный drawdown multiplier
17. [3.3] Calibration-aware conviction boost
18. [3.5] Экспоненциальный funding rate multiplier
19. [3.6] EMA-feedback loop для leverage adaptation
20. [3.7] Double precision intermediate leverage
21. [3.8] Cross-margin liquidation formula

### Фаза 4: Конфигурация (P2)
22. [6.2-6.5] Добавить все missing validations
23. [6.6] Ограничить max_leverage в production
24. [6.7-6.8] Исправить ports и initial_capital в configs
25. [6.9] Warning для unknown YAML keys
26. [5.6] Расширить YAML для ExtendedRiskConfig

### Фаза 5: Архитектура (P2)
27. [7.1] Декомпозировать on_feature_snapshot()
28. [7.2] Cache portfolio snapshot per tick
29. [7.5] Interface-only exchange module
30. [7.7] Conditional PostgreSQL
31. [4.1] Интегрировать resilience или удалить
32. [4.6] Интегрировать PositionSizer или удалить

### Фаза 6: Exchange API (P2)
33. [5.3] Stop orders (plan-order endpoint)
34. [5.4] Private WebSocket channel
35. [9.2] REST retry logic
36. [9.4] Binary search для leverage
37. [7.4] Connection pooling

### Фаза 7: Тестирование (P3)
38. [10.1-10.11] Все тесты по приоритету

### Фаза 8: Code cleanup (P3)
39. [4.2-4.10] Удалить/интегрировать dead code
40. [11.1-11.5] Code style fixes
41. [12.1-12.10] Calibrate coefficients с backtesting

---

**Общее количество задач: 80+**
**Оценочный объём изменений: ~50 файлов, ~5000-8000 строк**
