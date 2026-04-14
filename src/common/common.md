# Модуль common — подробный разбор

Временный рабочий документ.

Дата: 2026-04-10 (обновлён после production-grade аудита)

## 1. Назначение модуля

`src/common` — базовый инфраструктурный модуль проекта.

Он не содержит торговой логики. Его роль — задать общие типы, единый контракт ошибок, безопасные численные примитивы и фундаментальные правила, на которых строятся почти все остальные подсистемы.

Модуль решает пять задач:

1. вводит сильную типизацию для ключевых доменных значений;
2. задаёт единый безисключенческий контракт ошибок через `std::expected`;
3. централизует строковые представления доменных enum;
4. содержит общие численные guard-утилиты для индикаторов и ML;
5. хранит базовые константы для времени, комиссий и лимитов USDT-M futures, а также правила округления инструмента по биржевым ограничениям.

Архитектурно это один из самых низких слоёв проекта. Почти всё зависит от `common`, а сам `common` почти ни от чего не зависит.

## 2. Сборка и архитектурный статус

`CMakeLists.txt` собирает `tb_common` как `INTERFACE` библиотеку (C++23).

- в модуле нет `.cpp` файлов;
- весь код header-only;
- модуль не создаёт объектный код;
- все пользователи получают только include-path и compile-features.

Требование C++23 обусловлено использованием `std::expected` в `result.hpp`.

## 3. Состав модуля

| Файл | Назначение |
|---|---|
| `types.hpp` | строгие доменные типы (StrongType) и базовые enum |
| `enums.hpp` | преобразования enum ↔ строка |
| `errors.hpp` | системные коды ошибок и `std::error_code` интеграция |
| `result.hpp` | `Result<T>` / `VoidResult` на базе `std::expected` |
| `constants.hpp` | время, проценты, комиссии Bitget, биржевые лимиты |
| `exchange_rules.hpp` | precision/min-notional/min-qty правила инструмента |
| `numeric_utils.hpp` | NaN/Inf guards, safe arithmetic, валидация рядов |

По реальному использованию файлы неравнозначны:

- `types.hpp` — системное ядро, включается ~40 файлами;
- `result.hpp` + `errors.hpp` — основной error-path (~96 usage sites);
- `numeric_utils.hpp` — опорный слой для индикаторов и ML (~119 вызовов);
- `constants.hpp` + `exchange_rules.hpp` — фьючерсный торговый контур;
- `enums.hpp` — boundary/logging/config helper.

## 4. `types.hpp` — доменная сильная типизация

### 4.1. StrongType

```cpp
template<typename T, typename Tag>
class StrongType {
    T value_;
public:
    explicit constexpr StrongType(T value) noexcept(...);
    constexpr const T& get() const noexcept;
    constexpr T& get() noexcept;
    auto operator<=>(const StrongType&) const = default;
    bool operator==(const StrongType&) const = default;
};
```

Canonical strong typedef. Компилятор запрещает случайно подставить цену вместо количества.

### 4.2. Доменные типы

- `Symbol` — торговый инструмент (`BTCUSDT`)
- `Price` — цена в котируемой валюте
- `Quantity` — размер позиции/ордера
- `NotionalValue` — денежное выражение позиции
- `Timestamp` — wall-clock наносекунды от Unix-эпохи
- `StrategyId`, `StrategyVersion`, `ConfigHash` — метаданные стратегии
- `OrderId`, `TradeId`, `CorrelationId` — отдельные смысловые идентификаторы

### 4.3. Масштаб использования

`types.hpp` включается ~40 файлами в `src/`: execution, strategy, decision, risk, portfolio, recovery, reconciliation, logging, features, regime, world_model, uncertainty, pipeline, order_book, config, clock.

Это фактически общий язык всей системы.

### 4.4. Паттерн использования

Strong types используются как основной способ моделирования состояния:

- `OrderRecord.price` → `Price`
- `TradingPipeline::symbol_` → `Symbol`
- `LogEvent.timestamp` → `Timestamp`

На границах с внешним миром значения извлекаются через `.get()`.

### 4.5. Базовые enum

Futures-ориентированные перечисления:

- `Side` (Buy/Sell) — направление ордера
- `PositionSide` (Long/Short) — сторона позиции в hedge-mode
- `TradeSide` (Open/Close) — действие с позицией
- `OrderType` (Limit/Market/PostOnly/StopMarket/StopLimit)
- `TimeInForce` (GTC/IOC/FOK/GTD)
- `OrderStatus` (Pending/Open/PartiallyFilled/Filled/Cancelled/Rejected/Expired)
- `TradingMode` (Paper/Production)
- `RegimeLabel` (Trending/Ranging/Volatile/Unclear)
- `WorldStateLabel` (Stable/Transitioning/Disrupted/Unknown)
- `UncertaintyLevel` (Low/Moderate/High/Extreme)

Различие `PositionSide` и `TradeSide` — ключевая фьючерсная специфика для hedge-mode Bitget USDT-M.

### 4.6. `std::hash` specialization

Специализация для `unordered_map`/`unordered_set` — позволяет использовать strong types как ключи.

## 5. `enums.hpp` — строковая поверхность доменных enum

### 5.1. Структура

- `to_string(...)` для всех 10 enum (constexpr)
- `*_from_string(...)` для 8 enum (inline, возвращают `std::optional`)

Включает только `types.hpp` — минимальная зависимость.

### 5.2. Реальное использование (9 потребителей)

- `app/app_bootstrap.cpp`, `app/main.cpp` — парсинг режима торговли
- `pipeline/trading_pipeline.cpp` — structured logging
- `config/config_loader.cpp` — `trading_mode_from_string()`
- `execution/execution_engine_new.cpp` — логирование side/position_side
- `exchange/bitget/bitget_futures_order_submitter.cpp` — форматирование полей
- `risk/risk_engine.cpp`, `risk/policies/risk_checks.cpp` — логирование
- `leverage/leverage_engine.cpp` — логирование

### 5.3. Покрытие from_string

Покрыты: TradingMode, Side, PositionSide, TradeSide, OrderType, OrderStatus, RegimeLabel, UncertaintyLevel.

Не покрыты (не требуются на практике): TimeInForce, WorldStateLabel.

## 6. `errors.hpp` — системная таксономия ошибок

### 6.1. TbError enum (24 кода, 9 операционных зон)

- Конфигурация: ConfigLoadFailed, ConfigValidationFailed
- Безопасность: SecretNotFound, SecretProviderUnavailable
- Биржевое подключение: ExchangeConnectionFailed, ExchangeAuthFailed
- Рыночные данные: MarketDataStale, OrderBookOutOfSync
- Риск/исполнение: RiskDenied, ExecutionFailed
- Персистентность: PersistenceError, ReplayError
- Shadow: ShadowDisabled
- Reconciliation: ReconciliationFailed, ReconciliationMismatch
- Recovery: RecoveryFailed, RecoveryIncomplete
- Resilience: CircuitBreakerOpen, RetryExhausted, IdempotencyDuplicate
- WAL: WalWriteFailed, WalRecoveryFailed
- Координация: SymbolLockFailed, GlobalLimitExceeded
- Production safety: ProductionGuardFailed

### 6.2. `std::error_category` интеграция

`TbErrorCategory` реализует `std::error_category` с человекочитаемыми сообщениями.

`make_error_code(TbError)` + `is_error_code_enum<TbError>` — полная интеграция со стандартной error model.

### 6.3. Как ошибки текут по проекту

Через `result.hpp` (~96 usage sites):

- `ConfigLoadFailed` / `ConfigValidationFailed` в config
- `ProductionGuardFailed` в bootstrap
- `SecretNotFound` в security
- `PersistenceError` / `WalWriteFailed` в persistence
- `ExecutionFailed` / `RiskDenied` / `IdempotencyDuplicate` в execution
- `ExchangeConnectionFailed` / `ReconciliationFailed` в Bitget adapters

## 7. `result.hpp` — безисключенческий контракт операций

### 7.1. Основные типы

```cpp
template<typename T>
using Result = std::expected<T, TbError>;

using VoidResult = std::expected<void, TbError>;
```

### 7.2. Фабрики

- `Ok(value)` — успех
- `Err<T>(TbError)` — ошибка
- `OkVoid()` / `ErrVoid(TbError)` — для void-операций

### 7.3. Реальное использование

Широко используется (~96 вхождений): app_bootstrap, config_loader, config_validator, security providers, execution engine, cancel_manager, persistence layer, reconciliation.

Кодовая база приняла `std::expected`-модель и использует `Ok()`/`Err()` фабрики.

## 8. `constants.hpp` — централизованные базовые константы

### 8.1. Структура

```
tb::common::time            — временные константы (наносекунды)
tb::common::finance         — финансовые множители
tb::common::fees            — комиссии Bitget USDT-M Futures
tb::common::exchange_limits — лимиты Bitget USDT-M futures
```

### 8.2. Временные константы

| Константа | Значение | Обоснование |
|---|---|---|
| `kOneSecondNs` | 1'000'000'000 | SI: 10⁹ ns = 1 s |
| `kOneMinuteNs` | 60'000'000'000 | 60 × 10⁹ |
| `kFiveMinutesNs` | 300'000'000'000 | 5 × 60 × 10⁹ |
| `kOneHourNs` | 3'600'000'000'000 | 3600 × 10⁹ |
| `kOneDayNs` | 86'400'000'000'000 | 86400 × 10⁹ |

### 8.3. Финансовые множители

| Константа | Значение | Обоснование |
|---|---|---|
| `kPercentScaler` | 100.0 | Стандартное преобразование доля → проценты |
| `kBasisPointsScaler` | 10000 | 1 bps = 0.01% = 1/10000 (стандарт финансовой индустрии) |

### 8.4. Комиссии Bitget USDT-M Futures

| Константа | Значение | Источник |
|---|---|---|
| `kDefaultTakerFeePct` | 0.0006 (0.06%) | Bitget USDT-M Futures, Regular tier |
| `kDefaultMakerFeePct` | 0.0002 (0.02%) | Bitget USDT-M Futures, Regular tier |

Источник: Bitget Fee Schedule (USDT-M Futures, Regular tier, актуально на 2024-2025).

### 8.5. Лимиты биржи

| Константа | Значение | Обоснование |
|---|---|---|
| `kMinBitgetNotionalUsdt` | 1.10 | Абсолютный floor с 10% запасом от минимального notional ($1 для некоторых альткоинов). Фактический per-symbol минимум приходит через `ExchangeSymbolRules::min_trade_usdt`. |
| `kDustNotionalUsdt` | 0.50 | Порог пылевой позиции — ниже не стоит управлять. |

### 8.6. Реальное использование (10 потребителей)

execution (twap, order_registry, fill_processor, execution_engine), pipeline, portfolio, portfolio_allocator, risk (engine + checks).

## 9. `exchange_rules.hpp` — биржевые ограничения инструмента

### 9.1. ExchangeSymbolRules

Per-symbol runtime-правила, получаемые от биржи через exchange info API:

- `quantity_precision` — количество десятичных знаков (base)
- `price_precision` — количество десятичных знаков (quote)
- `min_trade_usdt` — минимальный notional (USDT), default 5.0
- `min_quantity` / `max_quantity` — границы quantity

### 9.2. Ключевые методы

| Метод | Назначение |
|---|---|
| `floor_quantity()` | Floor qty до допустимой точности (snprintf→strtod roundtrip против FP drift) |
| `round_price()` | Round half-up цены до допустимой точности |
| `is_quantity_valid()` | Проверка qty > 0, >= min, <= max |
| `is_notional_valid()` | Проверка notional >= min_trade_usdt |
| `validate_order()` | Полная валидация qty × price перед submit |
| `format_quantity()` / `format_price()` | Форматирование для REST payload |

### 9.3. Защита от UB

Precision значения клампируются в [0, 18] через `std::clamp()` перед использованием в `snprintf %.*f`. Это предотвращает UB при некорректных значениях precision.

### 9.4. Реальное использование (4 потребителя)

- `app/main.cpp` — инициализация rules из exchange info
- `pipeline/trading_pipeline.hpp` — валидация перед решением
- `execution/order_submitter.hpp` — formatting перед submit
- `exchange/bitget/bitget_futures_order_submitter.hpp` — form REST payload

## 10. `numeric_utils.hpp` — защита вычислительного контура

### 10.1. Назначение

Numeric firewall между сырыми данными и аналитикой. Предотвращает NaN/Inf propagation в индикаторах и ML.

### 10.2. Константы

| Константа | Значение | Обоснование |
|---|---|---|
| `kEpsilon` | 1e-9 | Стандартный eps для double comparison. IEEE 754 machine epsilon для double ≈ 2.2e-16; 1e-9 — conservative upper bound для торговых вычислений. |
| `kMinValidPrice` | 1e-10 | Floor для валидной цены. Ниже — считается нулём. |
| `kMinVariance` | 1e-12 | Floor для дисперсии. Предотвращает деление на ~0 в нормализации и z-score. |
| `kMaxReasonablePrice` | 1e12 | Upper bound $1T — выше любой реальной цены актива. |
| `kMaxReasonableVolume` | 1e15 | Upper bound — выше любого реального объёма. |
| `kDefaultStalenessNs` | 5s | Default порог устаревания. ML-модули переопределяют через config. |

### 10.3. Safe arithmetic

| Функция | Защита |
|---|---|
| `safe_div(num, den, fallback)` | den ≈ 0 или result не finite → fallback |
| `safe_clamp(v, lo, hi, fallback)` | NaN/Inf → fallback, иначе clamp |
| `safe_sqrt(v)` | v < 0 или не finite → 0.0 |
| `safe_log(v, fallback)` | v ≤ 0 или не finite → fallback |

### 10.4. Валидация

| Функция | Проверка |
|---|---|
| `is_finite(v)` | std::isfinite wrapper |
| `is_valid_price(p)` | finite ∧ p > kMinValidPrice ∧ p < kMaxReasonablePrice |
| `is_valid_volume(v)` | finite ∧ v ≥ 0 ∧ v < kMaxReasonableVolume |
| `validate_price_series(prices)` | все элементы — valid price |
| `validate_series_alignment(min, s1, s2, ...)` | одинаковая длина ≥ min |
| `is_stale(computed_at, now, threshold)` | (now - computed_at) > threshold |

### 10.5. Реальное использование (8 потребителей)

- `indicators/indicator_engine` — safe_div, safe_sqrt, validate_price_series, kEpsilon, kMinVariance
- `ml/correlation_monitor` — is_stale, kEpsilon
- `ml/bayesian_adapter` — is_stale, kEpsilon
- `ml/liquidation_cascade` — is_stale, kEpsilon
- `ml/entropy_filter` — is_stale, kEpsilon
- `ml/microstructure_fingerprint` — is_stale
- `ml/thompson_sampler` — is_stale

## 11. Аудит: что было удалено и почему

### 11.1. Удалённый файл: `ids.hpp`

**Причина:** 0 внешних включений, 0 внешних вызовов. Все функции (`generate_uuid_v4`, `generate_order_id`, `generate_trade_id`, `generate_correlation_id`, `generate_short_id`) — мёртвый код. Проект получает order_id с биржи, а не генерирует локально.

### 11.2. Удалённые макросы: `TB_TRY`, `TB_TRY_VOID`, `TB_TRY_VAL`

**Причина:** 0 внешних использований. Кроме того, `TB_TRY` содержал баг: `Err<decltype(auto)>` — невалидный тип. Кодовая база использует ручной unwrap `if (!result) return Err<...>(result.error())`.

### 11.3. Удалённые функции из `numeric_utils.hpp`

| Функция | Причина удаления |
|---|---|
| `is_bad(v)` | 0 внешних вызовов. Инверсия `is_finite()` без добавленной ценности. |
| `sanitize(v, fallback)` | 0 внешних вызовов. |
| `has_bad_values(data, n)` | 0 внешних вызовов. |
| `is_valid_spread_bps(s)` | 0 внешних вызовов. |
| `is_valid_probability(p)` | 0 внешних вызовов. |
| `is_valid_correlation(c)` | 0 внешних вызовов. |

### 11.4. Удалённые константы из `constants.hpp`

| Константа | Причина удаления |
|---|---|
| `finance::kFloatEpsilon` | 0 внешних использований. Дубликат `numeric::kEpsilon` (оба = 1e-9). |
| `finance::kMinValidPrice` | Дубликат `numeric::kMinValidPrice` (оба = 1e-10). 4 потребителя в portfolio_engine.cpp переведены на `numeric::kMinValidPrice`. |

### 11.5. Удалённые лишние includes

| Файл | Удалённый include | Причина |
|---|---|---|
| `result.hpp` | `<functional>` | Не использовался. |
| `enums.hpp` | `errors.hpp`, `result.hpp` | Не использовались. enums.hpp зависит только от types.hpp. |
| `numeric_utils.hpp` | `<type_traits>` | Не использовался после удаления мёртвых функций. |
| `exchange_rules.hpp` | `<algorithm>` | Не использовался (заменён на `std::clamp` с прямым include). |

### 11.6. Исправленные баги

| Файл | Баг | Исправление |
|---|---|---|
| `exchange_rules.hpp` | `snprintf %.*f` с неограниченным precision → потенциальный UB | Precision клампируется в [0, 18] через `std::clamp()` |
| `exchange_rules.hpp` | Отсутствует `<cstdlib>` для `std::strtod` | Добавлен include |
| `exchange_rules.hpp` | `min_trade_usdt` default 1.0 — занижен для большинства USDT-M pairs | default → 5.0 (стандарт Bitget для основных пар) |
| `CMakeLists.txt` | `cxx_std_20` при использовании `std::expected` (C++23) | → `cxx_std_23` |
| `secret_provider.hpp` | Отсутствует `<unordered_map>` (зависел от transitive include) | Добавлен прямой include |

## 12. Futures-only проверка

Модуль полностью ориентирован на USDT-M futures:

- `PositionSide` (Long/Short) и `TradeSide` (Open/Close) — futures hedge-mode семантика
- Комиссии названы явно как Bitget USDT-M Futures
- `ExchangeSymbolRules` — futures precision/min-notional submit path
- Лимиты биржи — Bitget USDT-M specific
- Нет spot-специфичных типов, enum или констант

## 13. Итоговое состояние модуля (7 файлов)

| Файл | Строк | Статус |
|---|---|---|
| `types.hpp` | ~170 | Production-ready |
| `enums.hpp` | ~175 | Production-ready |
| `errors.hpp` | ~130 | Production-ready |
| `result.hpp` | ~65 | Production-ready |
| `constants.hpp` | ~40 | Production-ready |
| `exchange_rules.hpp` | ~95 | Production-ready |
| `numeric_utils.hpp` | ~105 | Production-ready |

Сборка: 122/122 targets, 0 errors.
Тесты: 427/427 pass.
