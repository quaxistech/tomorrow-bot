# `src/risk/policies` — 37 политик риска

## Назначение

Реализации `IRiskCheck` для 37 ортогональных риск-политик. Каждая проверяет одно условие и обновляет общий `RiskDecision`.

## Список проверок (по объявлению в `risk_checks.hpp`)

| # | Class | Назначение |
|---|-------|------------|
| 1 | `KillSwitchCheck` | Emergency halt — единственный fast-path вход в EmergencyHalt action |
| 2 | `DayLockCheck` | Day-level lock |
| 3 | `SymbolLockCheck` | Per-symbol lock |
| 4 | `StrategyLockCheck` | Per-strategy lock |
| 5 | `SymbolCooldownCheck` | Cooldown после loss |
| 6 | `DailyLossCheck` | Дневной лимит потерь |
| 7 | `RealizedDailyLossCheck` | Реализованный day-loss |
| 8 | `MaxDrawdownCheck` | Account drawdown |
| 9 | `IntradayDrawdownCheck` | Intraday drawdown |
| 10 | `DrawdownHardStopCheck` | Hard-stop при глубокой просадке |
| 11 | `MaxPositionsCheck` | Лимит одновременно открытых позиций |
| 12 | `SameDirectionCheck` | Лимит позиций одного направления |
| 13 | `ExposureLimitCheck` | Лимит gross exposure |
| 14 | `PerTradeRiskCheck` | Per-trade risk budget |
| 15 | `MaxLeverageCheck` | Cap leverage |
| 16 | `MaxSlippageCheck` | Лимит ожидаемого slippage |
| 17 | `ConsecutiveLossesCheck` | Cooldown после серии losses |
| 18 | `MaxLossPerTradeCheck` | Cap loss-per-trade |
| 19 | `PerSymbolRiskCheck` | Per-symbol risk budget |
| 20 | `PerStrategyRiskCheck` | Per-strategy risk budget |
| 21 | `OrderRateCheck` | Rate-limit ордеров |
| 22 | `TurnoverRateCheck` | Rate-limit оборота |
| 23 | `TradeIntervalCheck` | Минимальный интервал между сделками |
| 24 | `StaleFeedCheck` | Деgnй market-data lag → блок entry/exit |
| 25 | `BookQualityCheck` | Invalid book → блок |
| 26 | `SpreadCheck` | Cap spread |
| 27 | `LiquidityCheck` | Cap liquidity |
| 28 | `UtcCutoffCheck` | UTC time cutoffs (e.g., funding hour) |
| 29 | `RegimeScaledLimitsCheck` | Regime-aware multipliers |
| 30 | `UncertaintyLimitsCheck` | Uncertainty=Extreme → блок |
| 31 | `UncertaintyCooldownCheck` | Cooldown после high-uncertainty период |
| 32 | `UncertaintyExecutionModeCheck` | Block при aggressive-style + high-uncertainty |
| 33 | `FundingRateCostCheck` | High funding cost relative to expected return |
| 34 | `VenueHealthCheck` | Reject rate, latency, reconnect frequency |
| 35 | `MarginDistanceCheck` | Distance to liquidation, margin headroom |
| 36 | `ReconciliationDesyncCheck` | **Production hardening 2026-05-08:** Local↔exchange mismatch — блокирует entry, разрешает close/reduce |
| 37 | `WsDisconnectedCheck` | **Production hardening 2026-05-08:** Public WS отвалился — блокирует все ордера (нет актуальной цены) |

> Точные имена и логика — в `risk_checks.hpp/cpp`. Все классы реализуют `IRiskCheck` и принимают `RiskState&`/`ExtendedRiskConfig` через ctor.

## Границы ответственности

* Каждый класс — **stateless** относительно решения; читает state из `RiskState` (через ref).
* Только PreTrade; intra-trade обрабатывается отдельно через `IntraTradeAssessment`.

## Зависимости

* `risk/risk_types.hpp`, `risk/risk_context.hpp`, `risk/state/risk_state.hpp`.
* `clock` (для timestamp-comparison checks).

## Потоки данных

`ProductionRiskEngine::init_checks` создаёт `unique_ptr<IRiskCheck>` для каждой проверки → `evaluate(ctx, decision)` в цикле.

## Race conditions

Под общим mutex'ом `RiskEngine::mutex_`.

## Ошибки проектирования

* **D-rskp-1 (LOW).** Все checks хранят ref на `ExtendedRiskConfig` / `RiskState` — lifetime owned by engine. Безопасно при текущем lifecycle, но нет защиты от dangling ref в misuse.
* **D-rskp-2 (LOW).** Порядок checks хардкоден в `init_checks` — изменение порядка может изменить hot-path latency и audit log distribution.

## Контракты

### `IRiskCheck::evaluate(ctx, decision)`

См. контракт в `risk/claude.md`.

## Рекомендации

1. Документировать порядок checks с rationale (почему KillSwitch первый — fast path).
2. Property-based тест: симуляция всех config-комбинаций на synthetic context.
3. Инфраструктура для добавления новых checks через регистрацию (factory) — снижает риск изменения существующих.
