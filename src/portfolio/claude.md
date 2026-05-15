# `src/portfolio` — Управление портфелем

## Назначение

In-memory учёт позиций, P&L, экспозиции, cash ledger и pending orders. Поддерживает hedge-mode (Long+Short одновременно на одном символе). Источник истины для downstream `PortfolioAllocator`, `RiskEngine`, `Decision`, `Reconciliation`.

## Границы ответственности

* CRUD позиций per (symbol, position_side).
* P&L: realized + unrealized + funding + fees.
* Cash ledger: total / available / reserved для pending orders.
* Pending orders: резервирование cash на момент отправки ордера.
* Аудит-лог событий портфеля (10K events ring).
* Sync с биржей через `sync_position_from_exchange` (идемпотентно).
* Дневные счётчики: trades_today, consecutive_losses, fees_accrued_today, peak_equity.
* Инварианты: `check_invariants()` для cash conservation.

## Публичные интерфейсы

* `class IPortfolioEngine` — interface (24 виртуальных метода).
* `class InMemoryPortfolioEngine` — реализация на `unordered_map<string, Position>`.
* DTO: `Position`, `ExposureSummary`, `PnlSummary`, `PortfolioSnapshot`, `CashLedger`, `PendingOrderInfo`, `PortfolioEvent`.

## Внутренние компоненты

* `portfolio_engine.hpp/cpp` — main.
* `portfolio_types.hpp` — DTO.

## Зависимости

* `common`, `clock`, `logging`, `metrics`.

## Потоки данных

```
ExecutionEngine::execute → reserve_cash(order_id, symbol, side, notional, fee)
  → cash_ledger_.available_cash -= reserve
  → pending_orders_[order_id] = info

submitter.submit → success → fill_event
  → release_cash(order_id) (по сути, заменяется на open_position)
  → open_position(Position)
       → positions_[make_key(sym, ps)] = pos
       → emit_event(PositionOpened)

Periodic update_price → unrealized P&L пересчёт
Reconciliation → sync_position_from_exchange
Closure → close_position(...) или reduce_position(...)
       → realized_pnl_today += amount
       → emit_event(PositionClosed)

Daily reset_daily → realized_pnl_today=0, trades_today=0, consecutive_losses=0, fees_accrued_today=0
```

## Race conditions

* `mutex_` под все mutating методы.
* `event_log_` ring (max 10K).

## Ошибки проектирования

* **D-pf-1 (HIGH).** `check_invariants()` опционален и не вызывается автоматически; нет периодической проверки cash conservation. См. R-9 в корне.
* **D-pf-2 (MEDIUM).** Hedge-mode key: `make_pos_key(symbol, side)` → `"SYMBOL:long"`/`"SYMBOL:short"`. Доступ к Position требует знания PositionSide. Старые vM (single-position) overload'ы делегируют в неявную семантику — risk при backward compat вызовах.
* **D-pf-3 (MEDIUM).** `reserve_cash` использует `notional` для расчёта margin, но не делит на leverage внутри портфеля — `leverage_` отдельное поле (`set_leverage`). Если leverage меняется между reserve и open_position — расхождение. Требует верификации.
* **D-pf-4 (LOW).** Default-implementations в interface (`record_funding_payment`/`record_fee`) — backward-compat hooks; новые реализации могут забыть override.

## Контракты

### `reserve_cash(order_id, symbol, side, notional, fee)`

* **Pre.** `order_id ∉ pending_orders`. `notional > 0 ∧ fee ≥ 0`. `cash_ledger_.available_cash ≥ notional/leverage_ + fee` (иначе → false).
* **Post.** `available_cash -= notional/leverage_ + fee`. `pending_orders[order_id] = {…}`. `pending_*_count` инкрементирован. Эмиттен `CashReserved` event.
* **Invariant.** Inv-5 (cash conservation): `available + reserved + Σposition_margin = total + Δpnl − fees`.

### `open_position(Position p)`

* **Pre.** `p.size > 0 ∧ p.avg_entry_price > 0 ∧ p.symbol != ""` ∧ Inv-2 (side/position_side симметрия).
* **Post.** `positions_[make_key(p.symbol, p.position_side)] = p`. `PositionOpened` event.
* **Invariant.** В hedge-mode разрешены 2 позиции на символ (Long+Short).

### `reduce_position(symbol, ps, sold_qty, close_price, realized_pnl) → remaining_size`

* **Pre.** Существует позиция `(symbol, ps)`. `sold_qty.get() > 0 ∧ ≤ position.size.get()`.
* **Post.** Если `sold_qty < pos.size` → `pos.size -= sold_qty`, return `remaining`. Иначе → удалить позицию, return 0. `realized_pnl_today += realized_pnl`. Event emitted.

### `sync_position_from_exchange(...)`

* **Pre.** Используется только из reconciliation/recovery.
* **Post.** Позиция в map обновлена/создана с exchange-truth values.
* **Invariant.** Идемпотентен: повторный вызов с теми же значениями — no-op.

## Производственные риски

* **R-pf-1.** Cash ledger drift из-за race / пропущенных fills → invariant break → реальный убыток через over-leverage. Mitigation: периодический `check_invariants()` + reconciliation.
* **R-pf-2.** `event_log_` ring 10K — при 50 ops/sec покрывает 200 секунд истории. Для post-mortem может не хватить. Альтернатива: дамп в persistence.

## Рекомендации

1. Сделать `check_invariants()` обязательным (вызов каждые N сделок); при нарушении → kill-switch.
2. Прямой proxy в `event_log_` через `persistence/event_journal` для долгосрочного аудита.
3. Метрики: `portfolio_cash_available`, `portfolio_open_positions`, `portfolio_realized_pnl_today`, `portfolio_drawdown_pct`.
4. Тест: симуляция 1000 случайных операций, проверка инварианта Inv-5 в каждой точке.
5. Удалить backward-compat overload'ы; требовать явного `position_side` везде.
