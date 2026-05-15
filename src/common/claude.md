# `src/common` — Базовый домен

## Назначение

Фундаментальные типы, ошибки и константы, на которых строятся все вышестоящие слои. Слой 0 в общей архитектуре. Не зависит ни от одного другого модуля кроме STL.

## Границы ответственности

* Strong-typing для домена (`Symbol`, `Price`, `Quantity`, `OrderId`, `Timestamp`, …).
* Перечисления (`Side`, `PositionSide`, `OrderType`, `OrderStatus`, `RegimeLabel`, `WorldStateLabel`, `UncertaintyLevel`, `TradingMode`).
* Иерархия ошибок: `TbError` + `TbErrorCategory` + `Result<T>` (alias для `std::expected<T, std::error_code>` / `tl::expected`).
* Биржевые константы и правила инструментов: `ExchangeSymbolRules`, `kDefaultTakerFeePct`, `kDefaultMakerFeePct`.
* Утилиты числовых операций: `numeric_utils.hpp` (clamp/round/ulp-сравнение).
* Reason-коды (`reason_codes.hpp`) для аудит-трейла.

## Входы / выходы

Не имеет runtime-IO. Чисто типы и `inline` функции.

## Публичные интерфейсы

* `template<typename T, typename Tag> class StrongType` — обёртка с `operator<=>` и явным конструктором.
* `enum class TbError` — все коды ошибок системы (24 кода).
* `class TbErrorCategory : std::error_category` — синглтон.
* `inline std::error_code make_error_code(TbError e)`.
* `is_error_code_enum<TbError> = true_type` — регистрация в `std`.

## Внутренние компоненты

* `types.hpp` — strong types, перечисления, `std::hash` специализации.
* `errors.hpp` — `TbError`, `TbErrorCategory`, `make_error_code`.
* `result.hpp` — `Result<T>`, `Ok(...)`, `Err(...)`, `VoidResult`.
* `enums.hpp` — `TradingMode`, `to_string(TradingMode)`.
* `constants.hpp` — комиссии, пороги по умолчанию.
* `exchange_rules.hpp` — `ExchangeSymbolRules` (precision, min_qty, min_notional, leverage limits).
* `numeric_utils.hpp` — `clamp_double`, `round_to_step`, `safe_divide`, `is_close`.
* `reason_codes.hpp` — `enum class ReasonCode` + `to_string`.

## Зависимости

* STL: `<string>`, `<cstdint>`, `<system_error>`, `<compare>`, `<functional>`, `<expected>` (или TL fallback).
* Никаких внешних библиотек.

## Потоки данных

`StrongType` — value type, копируется и перемещается. `make_error_code` — pure function. Все методы константные / `noexcept` где применимо.

## Race conditions

Отсутствуют по определению (нет state).

## Ошибки проектирования

* **D-common-1 (LOW).** `StrongType` не запрещает арифметику над не-числовыми типами через `get()` (нет SFINAE). Например, `Symbol::get()+Price::get()` валидно. Mitigation: добавить `requires`-ограничения для арифметических теговых типов.
* **D-common-2 (INFO).** В `TbError` отсутствуют отдельные коды для `OrderRejectedByExchange`, `MinNotionalViolated` — все мапятся в `ExecutionFailed`. Терять информацию для post-mortem.
* **D-common-3 (LOW).** Hash для `StrongType<std::string,Tag>` копирует строку при хэшировании? — реализация `std::hash<T>{}(v.get())` использует один уровень, без копии. ✓ OK.

## Контракты

### `StrongType<T,Tag>::StrongType(T value)`

* **Pre.** Никаких.
* **Post.** `get() == value` (после move/copy).
* **Invariant.** Тип `Tag` гарантирует, что разные домены не путаются на этапе компиляции.

### `make_error_code(TbError e)`

* **Pre.** `e` — валидный код перечисления.
* **Post.** Возвращён `error_code` с `value() == int(e) ∧ category() == TbErrorCategory::instance()`.
* **noexcept.** Да.

## Производственные риски

* **R-common-1.** При расширении `TbError` важно обновить и `TbErrorCategory::message`. Иначе для новых кодов в production логи получат «Неизвестная ошибка». Защита: unit-тест на полное покрытие `switch`.

## Рекомендации

1. Ввести SFINAE в `StrongType` для арифметических операторов только на числовых тегах.
2. Добавить отдельные коды ошибок для часто встречающихся биржевых отказов (см. `BitgetRestClient::error_code`).
3. Добавить unit-тест, проверяющий `TbError::message` для всех значений (`reflection over enum`).
