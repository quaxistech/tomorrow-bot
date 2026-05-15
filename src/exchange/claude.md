# `src/exchange` (родительский каталог)

## Назначение

Единая точка интеграции с биржевыми API. Содержит реализации специфические для конкретных бирж (на текущий момент — только Bitget).

## Структура

* `exchange/bitget/` — клиенты Bitget v2 API.

## Границы ответственности

* Адаптация внешних API к внутренним интерфейсам (`IOrderSubmitter`, `IExchangeQueryService`).
* Подписи (HMAC-SHA256), rate limiting, retry, clock sync.
* Только I/O — никакой business-логики.

## Зависимости

* `common`, `logging`, `metrics`, `resilience` (CircuitBreaker, RetryExecutor — TBD проверить уровень интеграции).

## Рекомендации

1. Если будет вторая биржа — выделить общий интерфейс на уровне `exchange/` (например, `IFuturesAdapter`) и спустить bitget-специфичность в submodule.
2. Сейчас интерфейсные `class IOrderSubmitter` и `IExchangeQueryService` живут в `execution/` и `reconciliation/` соответственно — это нарушает направленность зависимостей. Их желательно перенести в `exchange/` или `common/`.

> Подробности — в `src/exchange/bitget/claude.md`.
