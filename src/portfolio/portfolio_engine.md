# Подробный разбор модуля portfolio engine

Временный аналитический документ.

Источник разбора: текущая реализация `src/portfolio` и её реальные интеграции в `execution`, `pipeline` и `recovery` на момент 2026-04-09.
Это не продуктовая спецификация, а описание того, как модуль работает сейчас.

## 1. Что входит в модуль

Модуль `portfolio` состоит из трёх файлов:

| Файл | Назначение |
|---|---|
| `src/portfolio/CMakeLists.txt` | Собирает библиотеку `tb_portfolio` |
| `src/portfolio/portfolio_types.hpp` | Базовые структуры состояния портфеля, cash ledger, pending orders и event log |
| `src/portfolio/portfolio_engine.hpp/.cpp` | Интерфейс `IPortfolioEngine` и потокобезопасная in-memory реализация `InMemoryPortfolioEngine` |

Практически весь runtime-модуль сосредоточен в одном классе: `InMemoryPortfolioEngine`.

## 2. Главная роль модуля

`portfolio engine` отвечает за локальное состояние торгового капитала и позиций внутри бота.

Он хранит и пересчитывает:

1. открытые позиции по символам;
2. нереализованную и реализованную P&L;
3. экспозицию long/short;
4. cash-баланс и резерв под активные ордера;
5. серию убыточных сделок, drawdown и дневные счётчики;
6. аудит-лог событий портфеля.

Важно: модуль сам не исполняет ордера и сам не считает realized P&L по fill-ленте. Он получает уже рассчитанные значения от execution-слоя и фиксирует их у себя.

## 3. Базовые типы данных

### 3.1. `Position`

Одна позиция содержит:

- `symbol`;
- `side` (`Buy` или `Sell`), что в этой реализации играет роль long/short;
- `size`;
- `avg_entry_price`;
- `current_price`;
- `notional`;
- `unrealized_pnl` и `unrealized_pnl_pct`;
- `accumulated_funding` для фьючерсов;
- `strategy_id`;
- `opened_at`, `updated_at`.

Для USDT-M фьючерсов это означает, что модуль умеет хранить сторону позиции и учитывать funding, но всё ещё мыслит одной записью `Position` на символ.

### 3.2. `ExposureSummary`

Сводка экспозиции хранит:

- `gross_exposure`;
- `net_exposure`;
- `long_exposure`;
- `short_exposure`;
- `exposure_pct`;
- число открытых позиций.

### 3.3. `PnlSummary`

Сводка P&L хранит:

- `realized_pnl_today`;
- `unrealized_pnl`;
- `total_pnl`;
- `peak_equity`;
- `current_drawdown_pct`;
- `trades_today`;
- `consecutive_losses`.

### 3.4. `CashLedger` и `PendingOrderInfo`

Это отдельный слой учёта ликвидности:

- `total_cash` — полный USDT cash;
- `available_cash` — свободный cash;
- `reserved_for_orders` — резерв под активные открывающие ордера;
- `fees_accrued_today`;
- `realized_pnl_gross` и `realized_pnl_net`;
- агрегаты по pending orders.

`PendingOrderInfo` хранит резерв по каждому ордеру: order id, symbol, expected price, reserve cash, estimated fee, strategy и timestamp.

### 3.5. `PortfolioEvent`

Модуль ведёт event log по событиям:

- `PositionOpened`;
- `PositionClosed`;
- `PositionUpdated`;
- `PriceUpdated`;
- `CashReserved`;
- `CashReleased`;
- `FeeCharged`;
- `CapitalSynced`;
- `DailyReset`;
- `ReconciliationAdjustment`.

Фактически активно используются не все типы, но сам журнал нужен как audit trail.

## 4. Контракт `IPortfolioEngine`

Интерфейс делится на четыре группы операций.

### 4.1. Операции с позицией

- `open_position()`;
- `update_price()`;
- `record_funding_payment()`;
- `close_position()`;
- `reduce_position()`.

### 4.2. Получение состояния

- `get_position()`;
- `has_position()`;
- `snapshot()`;
- `exposure()`;
- `pnl()`.

### 4.3. Сервисные операции

- `reset_daily()`;
- `set_capital()`;
- `set_leverage()`.

### 4.4. Cash reserve API

- `reserve_cash()`;
- `release_cash()`;
- `record_fee()`;
- `cash_ledger()`;
- `pending_orders()`;
- `recent_events()`;
- `check_invariants()`.

Это означает, что `portfolio engine` в текущей архитектуре обслуживает не только позиции, но и внутренний ledger исполнения.

## 5. Внутреннее состояние `InMemoryPortfolioEngine`

Класс хранит:

- `total_capital_`;
- `leverage_`;
- `peak_equity_`;
- `realized_pnl_today_`;
- `trades_today_`;
- `consecutive_losses_`;
- `positions_` как `unordered_map<string, Position>`;
- `cash_ledger_`;
- `pending_orders_`;
- `event_log_`;
- один общий `mutex_`.

Ключевой вывод: это полностью in-memory snapshot-based движок без внешнего persistence внутри самого модуля.

## 6. Как работает lifecycle позиции

### 6.1. `open_position()`

Если позиции по символу нет, создаётся новая запись.

Если позиция уже есть и сторона совпадает, происходит additive merge:

- старый размер `old_size`;
- новый размер `new_size`;
- итоговый размер `total_size = old_size + new_size`;
- новая средняя цена входа считается как weighted average.

Формула:

$$
avg\_entry = \frac{old\_size \cdot old\_entry + new\_size \cdot new\_entry}{old\_size + new\_size}
$$

После этого пересчитывается unrealized P&L.

### 6.2. `update_price()`

Метод:

1. находит позицию по символу;
2. при `avg_entry_price <= 0` ставит текущую цену как стартовую цену входа для синхронизированной позиции;
3. обновляет `current_price`, `notional`, `updated_at`;
4. пересчитывает unrealized P&L;
5. обновляет `peak_equity_`, если equity стал выше исторического пика.

### 6.3. `record_funding_payment()`

Funding аккумулируется в `accumulated_funding`, после чего P&L пересчитывается заново.

Логика трактовки такая:

- положительный `funding_amount` означает, что позиция заплатила funding;
- отрицательный `funding_amount` означает, что позиция funding получила.

### 6.4. `close_position()`

Полное закрытие:

1. принимает `realized_pnl` извне;
2. увеличивает дневной realized P&L;
3. обновляет cash ledger;
4. обновляет `trades_today_` и `consecutive_losses_`;
5. удаляет позицию из `positions_`;
6. пересчитывает `peak_equity_`;
7. пишет событие и метрики.

Важно: сам модуль не вычисляет realized P&L для полного закрытия. Он доверяет вызывающему коду.

### 6.5. `reduce_position()`

Это partial close API.

Метод:

1. берёт `sold_qty` и обрезает её до реального размера позиции;
2. принимает `realized_pnl` тоже извне;
3. если остаток нулевой, удаляет позицию;
4. если остаток положительный, уменьшает size, обновляет price/notional и пересчитывает unrealized P&L;
5. при полном закрытии обновляет `trades_today_` и `consecutive_losses_`.

Практический смысл: execution-слой может отдавать в портфель и полное закрытие, и частичное сокращение позиции одним и тем же модулем.

## 7. Формулы расчётов

### 7.1. Unrealized P&L

Для long:

$$
PnL_{price} = (current - entry) \cdot size
$$

Для short:

$$
PnL_{price} = (entry - current) \cdot size
$$

Итоговая unrealized P&L:

$$
PnL_{unrealized} = PnL_{price} - accumulated\_funding
$$

### 7.2. P&L в процентах

$$
PnL\_{pct} = \frac{PnL_{unrealized}}{entry \cdot size} \cdot 100
$$

### 7.3. Exposure

- `gross_exposure = long_exposure + short_exposure`
- `net_exposure = long_exposure - short_exposure`

Для фьючерсов процент экспозиции считается не от полного notional, а от margin-adjusted exposure:

$$
margin\_exposure = \frac{gross\_exposure}{leverage}
$$

$$
exposure\_pct = \frac{margin\_exposure}{total\_capital} \cdot 100
$$

### 7.4. Drawdown

$$
current\_equity = total\_capital + realized\_pnl\_today + unrealized\_pnl
$$

$$
drawdown\_pct = \max\left(\frac{peak\_equity - current\_equity}{peak\_equity} \cdot 100, 0\right)
$$

### 7.5. Capital utilization

В `snapshot()` утилизация считается как:

$$
capital\_utilization = \frac{gross\_exposure / leverage + reserved\_for\_orders}{total\_capital} \cdot 100
$$

## 8. Cash reserve accounting

Это один из самых важных аспектов модуля для фьючерсной торговли.

### 8.1. `reserve_cash()`

Execution engine вызывает резерв перед отправкой открывающего ордера.

Метод:

1. считает `total_required = notional + estimated_fee`;
2. проверяет, что `available_cash` покрывает резерв полностью;
3. создаёт `PendingOrderInfo`;
4. уменьшает `available_cash`;
5. увеличивает `reserved_for_orders`.

### 8.2. `release_cash()`

При cancel/reject/fill резерв снимается:

- pending order удаляется;
- `available_cash` увеличивается;
- `reserved_for_orders` уменьшается.

### 8.3. `record_fee()`

Комиссия списывается отдельно:

- увеличивается `fees_accrued_today`;
- уменьшается `available_cash`;
- уменьшается `total_cash`;
- `realized_pnl_net` дополнительно уменьшается на величину комиссии.

Следствие: cash ledger в этой архитектуре ведёт net-P&L через отдельное списание fee, а не через уже очищенный realized P&L.

## 9. Что возвращает `snapshot()`

`snapshot()` собирает полную картину:

- копию всех позиций;
- exposure summary;
- pnl summary;
- cash ledger;
- список pending orders;
- `pending_buy_count`, `pending_sell_count`;
- `total_fees_today`;
- `capital_utilization_pct`;
- `computed_at`.

Важно: `available_capital` в snapshot берётся из `cash_ledger_.available_cash` без повторного вычитания margin.

## 10. Реальные интеграции

### 10.1. Pipeline

`TradingPipeline` создаёт `InMemoryPortfolioEngine`, задаёт стартовый капитал из конфига и затем синхронизирует leverage через `set_leverage()`.

### 10.2. Execution engine

Это главный потребитель модуля:

- перед открытием вызывает `reserve_cash()`;
- при reject/cancel вызывает `release_cash()`;
- при fill открывающего ордера вызывает `open_position()`, затем `release_cash()`, затем `record_fee()`;
- при fill закрывающего ордера вызывает `reduce_position()` и `record_fee()`.

### 10.3. Recovery

`RecoveryService` использует модуль для:

- восстановления `total_capital` из snapshot;
- replay событий `PositionOpened`, `PositionClosed`, `FeeCharged`, `CapitalSynced`.

## 11. Потокобезопасность

Почти все публичные методы защищены одним `std::mutex`.

Это даёт:

- простую модель согласованности;
- предсказуемые snapshot/exposure/pnl;
- отсутствие гонок между cash ledger и positions map.

Но цена за это — coarse-grained locking: весь портфель сериализован одним mutex.

## 12. Покрытие тестами

Модуль имеет отдельный набор unit-тестов `Portfolio`:

- открытие/получение позиции;
- unrealized P&L;
- realized P&L;
- exposure;
- drawdown;
- consecutive losses;
- `reset_daily()`;
- snapshot;
- `reserve_cash()` / `release_cash()` / `record_fee()`;
- event log;
- invariants;
- partial close через `reduce_position()`.

На момент анализа профильные 25 portfolio-тестов проходят.

## 13. Сильные стороны текущего дизайна

### 13.1. Портфель отделён от исполнения

Модуль не знает про REST, submitter, exchange API и остаётся чистым state engine.

### 13.2. Есть отдельный cash ledger

Это сильнее обычного «только позиции и P&L». Модуль понимает резерв, pending orders и комиссии.

### 13.3. Частичное закрытие встроено штатно

`reduce_position()` позволяет execution-слою управлять partial TP и partial close без дополнительного слоя.

### 13.4. Funding уже включён в модель фьючерсов

Для USDT-M это критично, и модуль это учитывает явно.

## 14. Главные ограничения текущей реализации

### 14.1. Одна позиция на символ

`positions_` индексируется по `symbol.get()`.

Следствие: модуль не может одновременно хранить long и short по одному и тому же инструменту. Для биржи в hedge mode это архитектурное ограничение.

### 14.2. `open_position()` не умеет безопасно неттировать противоположную сторону

Если по символу уже есть позиция противоположной стороны, новая запись просто перезапишет старую в map. Корректный netting или side-by-side hedge внутри самого модуля не реализован.

### 14.3. Realized P&L приходит извне

`close_position()` и `reduce_position()` не рассчитывают realized P&L самостоятельно. Они предполагают, что execution-слой передал уже корректное значение.

### 14.4. Pending order side неполно моделируется

`reserve_cash()` всегда создаёт `PendingOrderInfo` с `side = Buy`, потому что в интерфейсе нет параметра стороны.

Практический эффект:

- cash reserve работает;
- но `pending_sell_count` и `pending_sell_notional` фактически не отражают реальные short-open ордера как отдельную сторону.

### 14.5. `recompute_cash_ledger()` есть, но не используется

В модуле присутствует helper для полного пересчёта ledger, однако runtime-путь обновляет ledger инкрементально и этот метод нигде не вызывается.

## 15. Краткий итог

`portfolio engine` — это потокобезопасный локальный state engine, который ведёт позиции, P&L, drawdown и cash reserve для исполнения ордеров.

Он уже хорошо приспособлен под USDT-M фьючерсы за счёт:

- funding-aware P&L;
- leverage-aware exposure;
- cash reservation под margin;
- частичного закрытия позиции.

Но при этом у него есть важное архитектурное ограничение: модель данных остаётся `one symbol -> one position`, что упрощает pipeline, но не даёт полноценно представить hedge-mode с одновременным long и short по одному символу.