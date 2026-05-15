# `src/risk` — Риск-движок (33 проверки)

## Назначение

Финальный gate перед отправкой ордера на биржу. Цепочка policy-проверок (33 шт.) проверяет global / account / strategy / symbol / per-trade ограничения. Возвращает `RiskDecision` с `RiskAction ∈ {Allow, AllowWithReducedSize, Deny, DenyXLock, EmergencyHalt}`.

## Границы ответственности

* Kill switch / emergency halt.
* Lock'и: SymbolLock, StrategyLock, DayLock, AccountLock, EmergencyHalt.
* Cooldown'ы (per-symbol).
* Limits: max_daily_loss, drawdown, positions, exposure, concentration, same-direction, funding rate, volatility, etc.
* Per-strategy budgets.
* Regime-scaled thresholds.
* Min notional guard (per symbol from exchange rules).
* Audit trail: `decision.triggered_checks`, `decision.reasons`, `decision.summary`.
* Outputs aggregate: `RiskSnapshot`.
* Daily reset.

## Структура каталога

| Под-каталог | Назначение |
|-------------|------------|
| `policies/`   | 33 реализации `IRiskCheck` (per-policy classes) + `i_risk_check.hpp` |
| `state/`      | Stateful sub-components: `LockRegistry`, `LossStreakTracker`, `PnlTracker`, `DrawdownTracker` |

## Публичные интерфейсы

* `class IRiskEngine` — интерфейс (15 методов).
* `class ProductionRiskEngine` — реализация с цепочкой checks.
* `class IRiskCheck` — interface для одной проверки.
* `RiskDecision`, `RiskSnapshot`, `IntraTradeAssessment`, `RiskContext`, `ExtendedRiskConfig`.
* `enum RiskAction, RiskVerdict, RiskStateLevel, LockType, RiskPhase`.

## Внутренние компоненты

* `risk_engine.hpp/cpp` — main.
* `risk_types.hpp` — DTO.
* `risk_context.hpp` — input для checks.
* `policies/i_risk_check.hpp` — interface.
* `policies/risk_checks.hpp` — declarations всех 33 checks.
* `policies/*.cpp` — реализации.
* `state/risk_state.hpp/cpp` — `RiskState` (LockRegistry + LossStreak + PnlTracker + DrawdownTracker).

## Зависимости

* `strategy`, `portfolio`, `portfolio_allocator`, `features`, `execution_alpha`, `regime`, `uncertainty`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
evaluate(intent, sizing, portfolio, features, exec_alpha, uncertainty):
  lock(mutex_)
  ctx = build_RiskContext(...)
  decision = RiskDecision{Deny по умолчанию}
  for check in checks_:        // 33 штук
    if decision уже Deny + hard_block: skip (или продолжать для audit)
    check->evaluate(ctx, decision)   // обновляет decision
  finalize_decision(decision, intent, sizing, portfolio):
    apply min_notional guard (per-symbol)
    compute regime_scale
    aggregate summary
    emit metrics
  return decision

set_current_regime(regime):
  current_regime_ = regime
  regime_scale_factor_ = compute_factor(regime)

activate_kill_switch(reason): kill_switch_active_ = true
record_trade_result(is_loss):
  state_.loss_streak.record_trade_result(...)
record_trade_close(strategy, symbol, pnl):
  state_.pnl_tracker.record_trade_pnl(...)
  state_.loss_streak.record(...)
```

## Race conditions

* `mutex_` под весь evaluate.
* `kill_switch_active_`, `regime_scale_factor_`, `current_funding_rate_`, `min_notional_usdt_` атомарные.

## Ошибки проектирования

* **D-rsk-1 (HIGH).** Дублирующий `kill_switch_active_` атомарик в `ProductionRiskEngine`, синхронизируемый через listener. См. **Defect-D3 в корне**.
* **D-rsk-2 (MEDIUM).** Все 33 проверки выполняются последовательно даже при первой Deny — это нормально для audit, но удваивает latency hot-path.
* **D-rsk-3 (MEDIUM).** `RiskContext` строится из 6 snapshot'ов; копирование может быть дорогим. Mitigation: const-ref агрегатор.
* **D-rsk-4 (LOW).** Regime scale factor вычисляется как atomic, но используется в нескольких checks — race при concurrent `set_current_regime`.
* **D-rsk-5 (LOW).** `record_order_sent`/`record_trade_result`/`record_trade_close` — раздельные API; pipeline должен корректно вызывать каждый. Защиты от пропуска нет.

## Контракты

См. § 9 в корневом `claude.md`. Дополнительно:

### `IRiskCheck::evaluate(ctx, decision)`

* **Pre.** `ctx` валидный.
* **Post.** `decision` может быть обновлён: `triggered_checks.push_back(name)`, `warnings`/`hard_blocks` дополнены, `action` ужесточается (никогда не ослабляется).
* **Invariant.** Никогда не делает downstream-side effect (пишет состояние через `RiskState`).

### `set_current_regime(regime)`

* **Pre.** `regime` валидный.
* **Post.** `current_regime_ = regime`. `regime_scale_factor_` пересчитан.

## Производственные риски

* **R-rsk-1.** Race kill-switch (Defect-D3). Mitigation: см. § 13 R-3.
* **R-rsk-2.** При 33 проверках с разными config-параметрами — high config surface; mis-tuning одного параметра ломает trade-flow.
* **R-rsk-3.** Reset-daily критичен: если не вызывается на UTC-границе, `loss_streak`/`daily_pnl` накапливаются → ложные day-locks.

## Рекомендации

1. **R-rsk-Big.** Унифицировать kill-switch state с `Supervisor`.
2. Опциональный `early_exit` режим: при первом `EmergencyHalt` остановить chain (для production).
3. Property-based тесты на каждый `IRiskCheck`: при определённом `ctx` ожидаем определённое `decision.action`.
4. Метрика `risk_check_triggered_total{check}` per-check.
5. Документировать каждую из 33 проверок (что блокирует, при каких условиях, какой config-параметр).
