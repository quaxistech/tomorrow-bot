# `src/strategy` — Стратегический движок (скальпинг)

## Назначение

Единственная торговая стратегия системы — скальпинг с 4 внутренними сетапами (Momentum Continuation, Retest, Pullback in Microtrend, Rejection). Реализует `IStrategy` для backward compat. Внутри — state machine, setup detector/validator, position manager.

## Границы ответственности

* State machine SymbolState (11 состояний: Idle→Candidate→…→PositionOpen→…→Cooldown).
* Setup detection (4 типа).
* Setup validation (правила инвалидации).
* Position management (hold/reduce/exit).
* Build `TradeIntent` (НЕ ордер!).
* Notify-API: `notify_position_opened/closed/entry_rejected`.

## Структура каталога

| Под-каталог | Назначение |
|-------------|------------|
| `setups/`     | Setup detector + validator + lifecycle |
| `state/`      | Strategy state machine + setup models |
| `context/`    | Market context evaluator |
| `management/` | Position manager (hold/reduce/exit decisions) |

## Публичные интерфейсы

* `class IStrategy` (interface).
* `class StrategyEngine : IStrategy` — main.
* `StrategyContext` — input DTO (features + regime + world + uncertainty + position info + risk summary + HTF context).
* `TradeIntent` — output DTO (НЕ order). Содержит signal_intent, suggested_quantity, conviction, optional limit_price, setup_type, stop_reference.
* `StrategyMeta`, `StrategySignalType`, `SetupType`, `ExitReason`, `SymbolState`, `MarketContextQuality`.
* `class StrategyRegistry` — реестр (для multi-strategy сценариев, фактически 1).

## Внутренние компоненты

* `strategy_engine.hpp/cpp` — main `evaluate()`.
* `strategy_interface.hpp` — `IStrategy`.
* `strategy_registry.hpp/cpp` — registry.
* `strategy_config.hpp` — `ScalpStrategyConfig`.
* `strategy_types.hpp` — все DTO.
* `setups/setup_lifecycle.hpp/cpp` — `SetupDetector`, `SetupValidator`.
* `state/strategy_state.hpp/cpp` — `StrategyStateMachine`.
* `state/setup_models.hpp` — `Setup`, `SetupValidationResult`, `PositionManagementResult`.
* `context/market_context.hpp/cpp` — `MarketContextEvaluator` (quality scoring).
* `management/position_manager.hpp/cpp` — `PositionManager`.

## Зависимости

* `features`, `regime`, `world_model`, `uncertainty` (через `StrategyContext`).
* `common`.
* `clock`, `logging`.

## Потоки данных

```
TradingPipeline → strategy_->evaluate(StrategyContext)
  lock(mutex_)
  if state == Cooldown: handle_cooldown
  elif !position: handle_pre_entry
       → setup_detector_.detect → optional<Setup>
       → setup_validator_.validate / can_confirm
       → state machine transitions
       → если EntryReady → build_intent
  else: handle_position
       → position_manager_.evaluate → PositionManagementResult{action, exit_reason}
       → если выход → build_exit_intent
  return optional<TradeIntent>
```

## Race conditions

* `mutex_` сериализует все operations (evaluate + notify_*).
* `active_` атомарный.
* `last_reasons_` под mutex.

## Ошибки проектирования

* **D-str-1 (MEDIUM).** Один глобальный `mutex_` сериализует все вызовы; pipeline блокируется на длительность `evaluate` (включая внутренние подсчёты setup'ов). Можно сделать lock-free через snapshot-pattern.
* **D-str-2 (MEDIUM).** State machine не имеет `force_reset` для аварийных ситуаций (только `reset()` который сбрасывает всё). При reconciliation mismatch может потребоваться surgical fix.
* **D-str-3 (LOW).** `last_reasons_` хранит только последний эпизод — нет ring-buffer истории решений.
* **D-str-4 (LOW).** `notify_entry_rejected` не различает типы отказа (sizing/risk/exchange) — все одинаково.

## Контракты

### `StrategyEngine::evaluate(StrategyContext ctx)`

* **Pre.**
  * `ctx.features.symbol != ""`.
  * `ctx.features.is_stale = false`.
  * `ctx.is_strategy_enabled = true` (иначе `nullopt` без работы).
* **Post.**
  * Возвращён `optional<TradeIntent>`.
  * `last_reasons_` содержит причину текущего решения (для explainability).
* **Invariant.**
  * State machine переходит между состояниями только по разрешённым ребрам.
  * После `EnterLong/Short` → состояние `EntrySent` (не `PositionOpen` до `notify_position_opened`).

### `notify_position_opened(price, size, side, position_side)`

* **Pre.** Был сгенерирован `EnterLong`/`EnterShort` ранее. State = `EntrySent`.
* **Post.** State → `PositionOpen`. Position info обновляется в внутреннем state.

### `notify_position_closed()`

* **Pre.** State ∈ {`PositionOpen`, `PositionManaging`, `ExitPending`}.
* **Post.** State → `Cooldown`. Reset trailing-related state.

## Производственные риски

* **R-str-1.** State machine drift: если `notify_*` не вызывается из pipeline (например, ошибка в pipeline), strategy остаётся в `EntrySent` навсегда. Mitigation: timeout watchdog в strategy ИЛИ форсирование reset через external API.
* **R-str-2.** Setup detection может срабатывать «слишком часто» в нестабильном рынке (ложные сигналы) — управление через `MarketContextEvaluator`.

## Рекомендации

1. Per-state mutex или snapshot-pattern для уменьшения contention.
2. Watchdog для зависших состояний (timeout → reset + alert).
3. Ring-buffer истории решений для post-mortem.
4. Различимые `notify_entry_rejected_for_*` (по причине) — для статистики причин отказа.
5. Property-based тесты на state machine (transitions всегда корректны).
