# Подробный разбор модуля risk

Временный аналитический документ.

Источник разбора: текущая реализация `src/risk`, реальные вызовы из `pipeline`, конфигурационный маппинг из `config`, а также unit-тесты на момент 2026-04-13.
Это описание фактической реализации после полной ревизии модуля.

## 1. Что входит в модуль

Модуль `risk` состоит из десяти основных файлов и трёх подкаталогов:

| Файл | Назначение |
|---|---|
| `src/risk/CMakeLists.txt` | Сборка библиотеки `tb_risk` |
| `src/risk/risk_context.hpp` | Единый входной контекст для policy-checks |
| `src/risk/risk_types.hpp` | Доменные типы решения, состояния и полной конфигурации |
| `src/risk/risk_types.cpp` | `to_string()` для enum-типов risk-модуля |
| `src/risk/risk_engine.hpp` | Интерфейсы `IRiskEngine` и `ProductionRiskEngine` |
| `src/risk/risk_engine.cpp` | Оркестрация проверок, state lifecycle, snapshot, kill switch |
| `src/risk/policies/i_risk_check.hpp` | Интерфейс одной policy-проверки |
| `src/risk/policies/risk_checks.hpp` | Объявления всех policy-checks |
| `src/risk/policies/risk_checks.cpp` | Реальная логика всех проверок |
| `src/risk/state/risk_state.*` | Locks, streaks, pnl, drawdown, rate trackers |

Зависимости по CMake:

- `tb_common`
- `tb_strategy`
- `tb_features`
- `tb_portfolio`
- `tb_portfolio_allocator`
- `tb_execution_alpha`
- `tb_logging`
- `tb_clock`
- `tb_metrics`
- `tb_regime`
- `tb_uncertainty`

Практически вся рабочая логика сосредоточена в трёх слоях:

1. `ProductionRiskEngine` — orchestration layer;
2. `IRiskCheck` + `risk_checks.cpp` — policy layer;
3. `RiskState` + trackers — stateful memory layer.

## 2. Главная роль модуля

Модуль `risk` выполняет три разные функции:

1. **Pre-trade gating** — можно ли пускать новый entry/TWAP-slice дальше в execution;
2. **Intra-trade monitoring** — нужно ли закрыть уже открытую позицию из-за времени удержания или adverse excursion;
3. **Post-trade accounting** — обновление внутренних лимитов, cooldowns, streaks, strategy budgets и drawdown state.

Иными словами, это не просто «ещё одна проверка перед ордером», а stateful risk-control слой между:

- `portfolio_allocator`;
- `execution_alpha`;
- `uncertainty`;
- `portfolio`;
- `pipeline`.

Для текущего USDT-M futures бота `risk` принимает уже подготовленный `TradeIntent`, уже уменьшенный аллокатором размер и уже рассчитанный `ExecutionAlphaResult`, после чего принимает финальное risk-решение.

## 3. Публичный контракт модуля

## 3.1. `IRiskEngine`

Публичный интерфейс у `risk` шире, чем у `execution_alpha`:

- `evaluate(...)` — pre-trade решение;
- `activate_kill_switch(...)` / `deactivate_kill_switch()` / `is_kill_switch_active()`;
- `record_order_sent()` — обновление rate limits;
- `record_trade_result(bool)` — делегирует в `record_trade_close()`;
- `record_trade_close(...)` — детальная фиксация закрытой сделки;
- `evaluate_position(...)` — intra-trade оценка уже открытой позиции;
- `set_current_regime(...)` — обновление regime-aware scaling;
- `set_funding_rate(double)` — обновление live funding rate для FundingRateCostCheck;
- `get_risk_snapshot()` — observability snapshot;
- `reset_daily()` — сброс дневного состояния.

Это подчёркивает, что `risk` — не pure-function модуль. Он хранит состояние и живёт между тиками.

## 3.2. `RiskContext`

Все policy-checks получают единый `RiskContext`:

- `strategy::TradeIntent intent`;
- `portfolio_allocator::SizingResult sizing`;
- `portfolio::PortfolioSnapshot portfolio`;
- `features::FeatureSnapshot features`;
- `execution_alpha::ExecutionAlphaResult exec_alpha`;
- `uncertainty::UncertaintySnapshot uncertainty`;
- `double current_funding_rate`.

Ключевой практический смысл:

- `risk` использует **и** данные портфеля,
- **и** execution-оценку,
- **и** uncertainty-рекомендации,
- **и** market-data validity.

`current_funding_rate` передаётся через `std::atomic<double> current_funding_rate_`, обновляемый вызовом `set_funding_rate()` из pipeline каждые 5 минут.

## 3.3. `RiskDecision`

Главный результат модуля — `RiskDecision`.

Он содержит сразу несколько слоёв информации:

### Legacy-поля

- `verdict`;
- `approved_quantity`;
- `risk_utilization_pct`;
- `kill_switch_active`;
- `summary`;
- `phase`.

### Расширенные policy-based поля

- `action`;
- `allowed`;
- `original_size`;
- `risk_state`;
- `triggered_checks`;
- `warnings`;
- `hard_blocks`;
- `reasons`.

### Audit / observability поля

- `current_daily_pnl`;
- `current_drawdown_pct`;
- `current_gross_exposure`;
- `active_locks_count`;
- `regime_scaling_factor`;
- `strategy_budget_utilization_pct`;
- `symbol_concentration_pct`.

Важно: `approved_quantity` инициализируется уже из `SizingResult.approved_quantity`, то есть `risk` работает не с исходным размером стратегии, а с размером, который уже прошёл через `portfolio_allocator`.

## 3.4. `ExtendedRiskConfig`

`ExtendedRiskConfig` содержит лимиты:

- per-trade;
- per-symbol;
- per-strategy;
- daily;
- drawdown;
- exposure;
- rate limiting;
- regime-aware scaling;
- intra-trade;
- kill-switch;
- funding.

Все поля реально используются проверками. Неиспользуемые поля и dormant sizing-конфигурация были удалены.

Но важно разделять:

1. **что тип умеет хранить**;
2. **что реально маппится из конфигурации**;
3. **что реально используется проверками**.

В текущем коде это три разные множества.

## 4. Как работает `ProductionRiskEngine`

## 4.1. Конструктор

Конструктор принимает:

- `ExtendedRiskConfig`;
- `ILogger`;
- `IClock`;
- `IMetricsRegistry`.

Инициализирует:

- `state_`;
- policy-chain через `init_checks()`.

## 4.2. Реальное количество проверок

Модуль содержит **33** policy-checks. Комментарии в коде, документация и `checks_.reserve(33)` согласованы с фактическим количеством.

## 4.3. `evaluate()` пошагово

Главная pre-trade точка входа устроена так.

### Шаг 1. Блокировка на уровне движка

Весь `evaluate()` защищён `std::lock_guard`, то есть risk engine работает как stateful serial section.

### Шаг 2. Обновление drawdown state

Перед проверками вызывается:

$$
state_.drawdown.update\_equity(portfolio.total\_capital, now)
$$

Это обновляет внутренний peak/intraday peak tracker.

### Шаг 3. Очистка истёкших lock-ов

`state_.locks.clear_expired(now)` удаляет cooldown/временные lock-ы, у которых истекла `duration_ns`.

### Шаг 4. Начальная инициализация решения

`RiskDecision` заполняется как optimistic-allow:

- `verdict = Approved`;
- `action = Allow`;
- `allowed = true`;
- `approved_quantity = sizing.approved_quantity`;
- `original_size = sizing.approved_quantity`.

То есть все проверки дальше модифицируют уже существующее решение.

### Шаг 5. Сбор контекста

Собирается `RiskContext` из 6 переданных аргументов.

Важно: `current_funding_rate` передаётся из `current_funding_rate_.load()` — атомарного поля, обновляемого через `set_funding_rate()` из pipeline.

### Шаг 6. Последовательный прогон policy-chain

Дальше выполняется простой цикл:

```text
for (auto& check : checks_) {
    check->evaluate(ctx, decision);
}
```

Это означает:

- short-circuit после первого deny нет;
- модуль собирает сразу несколько причин;
- порядок проверок влияет на итоговое `action`, список `hard_blocks` и конечный размер.

### Шаг 7. Синхронизация `verdict ↔ action ↔ allowed`

После цикла движок вручную синхронизирует состояние:

- `Denied` → `allowed = false`, при обычном action он заменяется на `Deny`;
- `Throttled` → `allowed = false`;
- `ReduceSize` → `action = AllowWithReducedSize`;
- иначе остаётся `allowed = true`.

### Шаг 8. `finalize_decision()`

На финализации выполняются ещё несколько вещей.

#### 8.1. Min notional guard после ReduceSize

Если размер был уменьшен, движок оценивает новый нотионал:

$$
price\_per\_unit = \frac{sizing.approved\_notional}{sizing.approved\_quantity}
$$

$$
reduced\_notional = decision.approved\_quantity \cdot price\_per\_unit
$$

Если он ниже `kMinBitgetNotionalUsdt`, решение превращается в `Denied`.

#### 8.2. Risk utilization

В `decision.risk_utilization_pct` кладётся:

$$
\frac{portfolio.exposure.gross\_exposure}{portfolio.total\_capital}
$$

Важно: это **текущая** утилизация до нового ордера, а не пост-трейд проекция.

#### 8.3. Summary и metrics

Формируется строка summary, логируются non-approved решения и инкрементируется метрика `risk_evaluations_total`.

## 5. Внутренние helper-семантики

В `risk_checks.cpp` есть четыре helper-функции:

- `deny(...)`;
- `deny_lock(...)`;
- `reduce(...)`;
- `throttle(...)`.

Особенно важно поведение `reduce(...)`:

- если verdict был `Approved`, он переводится в `ReduceSize`;
- `approved_quantity` устанавливается как `min(approved_quantity, original_size * ratio)`;
- добавляется warning-причина.

Семантика `reduce()` — **min-семантика**:
при нескольких срабатываниях итоговый размер = `min(original_size × ratio_1, original_size × ratio_2, …)`.
Это предотвращает мультипликативное компаундирование и даёт корректный результат, соответствующий самому строгому лимиту.

## 6. Все 33 policy-checks

Ниже — фактический порядок выполнения.

## 6.1. Блокировки и lock-based ограничения

### 1. `KillSwitchCheck`

Смотрит `state_.locks.has_emergency_halt()`.

Результат:

- `Denied`;
- `action = EmergencyHalt`;
- reason code: `KILL_SWITCH`.

### 2. `DayLockCheck`

Смотрит `state_.locks.has_day_lock()`.

Результат:

- `Denied`;
- `action = DenyDayLock`;
- code: `DAY_LOCK`.

### 3. `SymbolLockCheck`

Проверяет блокировку по конкретному символу.

Код: `SYMBOL_LOCK`.

### 4. `StrategyLockCheck`

Проверяет блокировку по `strategy_id`.

Код: `STRATEGY_LOCK`.

### 5. `SymbolCooldownCheck`

Если по символу активен cooldown, решение становится `Throttled`, а не `Denied`.

Код: `SYMBOL_COOLDOWN`.

## 6.2. Дневные и drawdown-лимиты

### 6. `DailyLossCheck`

Использует `portfolio.pnl.total_pnl`.

Формула:

$$
loss\_pct = \frac{|\min(total\_pnl, 0)|}{total\_capital} \cdot 100
$$

Это означает, что учитывается не только realized PnL, но и текущий суммарный дневной убыток, включая floating loss.

Код: `MAX_DAILY_LOSS`.

### 7. `RealizedDailyLossCheck`

Использует `portfolio.pnl.realized_pnl_today`.

Формула:

$$
realized\_loss\_pct = \frac{|\min(realized\_pnl\_today, 0)|}{total\_capital} \cdot 100
$$

Код: `REALIZED_DAILY_LOSS`.

### 8. `MaxDrawdownCheck`

Использует `portfolio.pnl.current_drawdown_pct` из внешнего snapshot.

Код: `MAX_DRAWDOWN`.

### 9. `IntradayDrawdownCheck`

Использует **внутренний** `state_.drawdown.intraday_drawdown_pct()`, а не поле портфеля.

Код: `INTRADAY_DRAWDOWN`.

### 10. `DrawdownHardStopCheck`

Если `portfolio.pnl.current_drawdown_pct >= drawdown_hard_stop_pct`, то:

- решение становится `Denied` с `action = EmergencyHalt`;
- в `LockRegistry` добавляется `EmergencyHalt` lock.

Код: `DRAWDOWN_HARD_STOP`.

## 6.3. Позиционные и экспозиционные ограничения

### 11. `MaxPositionsCheck`

Сравнивает `portfolio.exposure.open_positions_count` с `max_concurrent_positions`.

Код: `MAX_POSITIONS`.

### 12. `SameDirectionCheck`

Считает количество current long/short-позиций через `portfolio.positions`.

Проверяет три разных лимита:

- `max_simultaneous_long_positions`;
- `max_simultaneous_short_positions`;
- `max_same_direction_positions`.

Коды:

- `MAX_LONG_POSITIONS`;
- `MAX_SHORT_POSITIONS`;
- `SAME_DIRECTION`.

### 13. `ExposureLimitCheck`

Проецирует post-trade экспозицию:

$$
projected\_gross = gross\_exposure + approved\_notional
$$

$$
gross\_pct = \frac{projected\_gross}{total\_capital} \cdot 100
$$

Код: `MAX_EXPOSURE`.

Это предотвращает пробой лимита на последнем ордере.

### 14. `PerTradeRiskCheck`

По факту это не full per-trade risk model, а просто cap по notional:

$$
if\ approved\_notional > max\_position\_notional \Rightarrow reduce
$$

Код: `MAX_NOTIONAL`.

Важно: название проверки шире, чем её фактическая логика.

### 15. `MaxLeverageCheck`

Проецирует post-trade leverage:

$$
leverage = \frac{gross\_exposure + approved\_notional}{total\_capital}
$$

$$
effective\_max = max\_leverage \cdot (1 - liquidation\_buffer\_pct/100)
$$

Код: `MAX_LEVERAGE`.

### 16. `MaxSlippageCheck`

Смотрит `exec_alpha.quality.estimated_slippage_bps`.

Код: `MAX_SLIPPAGE`.

## 6.4. Streaks, symbol и strategy budgets

### 17. `ConsecutiveLossesCheck`

Использует **не внутренний state**, а `portfolio.pnl.consecutive_losses`.

Проверяет:

- `max_consecutive_losses` → `CONSECUTIVE_LOSSES`;
- `halt_after_n_losses` → `HALT_AFTER_LOSSES` c `DenyAccountLock`.

То есть здесь один из источников истины — сам `PortfolioSnapshot`, а не `state_.loss_streaks`.

### 18. `MaxLossPerTradeCheck`

Сканирует все уже открытые позиции и вычисляет для каждой:

$$
loss\_pct = \frac{|unrealized\_pnl|}{total\_capital} \cdot 100
$$

Если какая-то позиция теряет больше лимита, новый ордер блокируется.

Код: `MAX_LOSS_PER_TRADE`.

### 19. `PerSymbolRiskCheck`

Проверяет три вещи:

1. концентрацию по символу;
2. streak losses по символу из `state_.loss_streaks`;
3. daily PnL по символу из `state_.pnl`.

Формула концентрации (проецирует post-trade):

$$
conc\_pct = \frac{symbol\_exposure + approved\_notional}{total\_capital} \cdot 100
$$

Коды:

- `SYMBOL_CONCENTRATION`;
- `SYMBOL_CONSECUTIVE_LOSSES`;
- `SYMBOL_DAILY_LOSS`.

### 20. `PerStrategyRiskCheck`

В текущем коде эта проверка **не** оценивает strategy exposure и drawdown, несмотря на название и комментарий.

Она делает только одно:

- смотрит `state_.strategy_budgets[strategy_id].daily_loss`;
- переводит это в `% капитала`;
- сравнивает с `max_strategy_daily_loss_pct`.

Код: `STRATEGY_BUDGET`.

Дополнительный нюанс: `daily_loss` в budget накапливается как сумма **только отрицательных** результатов по модулю. Прибыльные сделки его не уменьшают.

То есть это не net PnL стратегии за день, а gross loss budget.

## 6.5. Rate limiting

### 21. `OrderRateCheck`

Использует `state_.rates.orders_last_minute(now)`.

Код: `ORDER_RATE`.

### 22. `TurnoverRateCheck`

Использует `state_.rates.trades_last_hour(now)`.

Код: `TURNOVER_RATE`.

### 23. `TradeIntervalCheck`

Смотрит timestamp последней закрытой сделки по символу.

Если прошло меньше `min_trade_interval_ns`, выдаёт `Throttled`.

Код: `TRADE_INTERVAL`.

## 6.6. Market-data и microstructure guards

### 24. `StaleFeedCheck`

Проверяет два условия:

1. `features.execution_context.is_feed_fresh`;
2. `features.market_data_age_ns <= max_feed_age_ns`.

Код: `STALE_FEED`.

### 25. `BookQualityCheck`

Требует `features.book_quality == BookQuality::Valid`.

Код: `INVALID_BOOK`.

### 26. `SpreadCheck`

Проверяет `features.microstructure.spread_bps > max_spread_bps`.

Код: `WIDE_SPREAD`.

### 27. `LiquidityCheck`

Использует суммарную L5 depth:

$$
total\_depth = bid\_depth\_5\_notional + ask\_depth\_5\_notional
$$

Если `total_depth < min_liquidity_depth`, вход блокируется.

Код: `LOW_LIQUIDITY`.

## 6.7. Timing, regime и uncertainty

### 28. `UtcCutoffCheck`

Если `utc_cutoff_hour >= 0`, торговля блокируется после указанного часа UTC.

Код: `UTC_CUTOFF`.

### 29. `RegimeScaledLimitsCheck`

Берёт `regime_scale_factor_`, установленный через `set_current_regime()`.

Если scale < 1.0, максимальный допустимый notional становится:

$$
scaled\_max = max\_position\_notional \cdot scale
$$

Если `ctx.sizing.approved_notional > scaled_max`, размер уменьшается.

Код: `REGIME_SCALED_LIMIT`.

### 30. `UncertaintyLimitsCheck`

Работает только для `High` и `Extreme` uncertainty.

Допустимый notional:

$$
adj\_max = max\_position\_notional \cdot uncertainty.size\_multiplier
$$

Если sizing больше, выдаётся reduce.

Код: `UNCERTAINTY_LIMIT`.

### 31. `UncertaintyCooldownCheck`

Если `uncertainty.cooldown.active == true`, решение становится `Throttled`.

Код: `UNCERTAINTY_COOLDOWN`.

### 32. `UncertaintyExecutionModeCheck`

Если `execution_mode == HaltNewEntries` и `intent.trade_side == Open`, новый вход блокируется.

Код: `UNCERTAINTY_HALT`.

## 6.8. Funding rate

### 33. `FundingRateCostCheck`

Вычисляет годовую стоимость фандинга:

$$
annual\_cost\_pct = |funding\_rate| \cdot 3 \cdot 365 \cdot 100
$$

Если она выше `max_annual_funding_cost_pct`, вход блокируется.

Код: `FUNDING_COST_EXCESSIVE`.

`current_funding_rate` поступает в `RiskContext` из атомарного поля `current_funding_rate_`, обновляемого pipeline через `set_funding_rate()` каждые 5 минут от Bitget.

## 7. Подсистема состояния `RiskState`

## 7.1. `LockRegistry`

Хранит `LockRecord` и умеет:

- добавлять lock;
- удалять lock;
- чистить истёкшие;
- проверять symbol/strategy/day/account/emergency/cooldown lock-и;
- вычислять глобальный уровень состояния.

Но есть важный нюанс: `compute_global_state()` никогда не возвращает `RiskStateLevel::SymbolLock` или `RiskStateLevel::StrategyLock` напрямую. При наличии symbol/strategy/cooldown lock-ов он возвращает общий `Degraded`.

То есть enum богаче, чем фактическая логика агрегирования.

## 7.2. `LossStreakTracker`

Хранит:

- global consecutive losses;
- daily stopouts;
- last loss time;
- per-symbol streaks;
- per-strategy streaks.

При прибыльной сделке глобальный streak и streak-и по текущему symbol/strategy обнуляются.

## 7.3. `PnlTracker`

Ведёт:

- `daily_realized_pnl_`;
- `trades_today_`;
- per-symbol realized pnl;
- per-strategy realized pnl.

Это отдельный state-source, не совпадающий с `StrategyRiskBudget.daily_loss`.

## 7.4. `DrawdownTracker`

Хранит:

- `peak_equity_`;
- `current_equity_`;
- `intraday_peak_`.

Формулы:

$$
account\_drawdown = \frac{peak\_equity - current\_equity}{peak\_equity} \cdot 100
$$

$$
intraday\_drawdown = \frac{intraday\_peak - current\_equity}{intraday\_peak} \cdot 100
$$

## 7.5. `RateTracker`

Хранит deque timestamp-ов для:

- order rate за минуту;
- closed trades за час;
- последнюю сделку по каждому символу.

## 8. Удалённые компоненты

### 8.1. `PositionSizer` (удалён)

Ранее в модуле существовал `PositionSizer`, который умел вычислять `SizingAdjustment` по трём факторам: volatility, liquidity, drawdown proximity. Однако он никогда не использовался в `ProductionRiskEngine::evaluate()` — это был dormant code. Файлы `sizing/position_sizer.hpp` и `sizing/position_sizer.cpp` удалены.

### 8.2. `SpotSellCheck` (удалён)

Legacy spot-guard, проверявший наличие открытой long-позиции перед SELL. Несовместим с USDT-M futures, где short-entry — штатная операция. Удалён из policy chain.

### 8.3. Неиспользуемые поля `ExtendedRiskConfig` (удалены)

Удалено ~15 полей, которые существовали в типе, но не использовались ни одной проверкой:
`max_risk_per_trade_pct`, `max_risk_per_trade_abs`, `max_position_notional_per_symbol`, `max_daily_loss_per_symbol_abs`, `max_strategy_drawdown_pct`, `max_strategy_exposure_pct`, `max_positions_per_strategy`, `max_daily_loss_abs`, `max_daily_stopouts`, `max_net_directional_exposure_pct`, `kill_switch_on_data_stale`, `kill_switch_on_position_mismatch`, `allow_reduce_only_in_halt`, все `sizer_*` поля.

### 8.4. Удалённые enum-значения

`ForceReduce` и `ForceLiquidate` удалены из `RiskAction` — они не использовались ни одной проверкой.

### 8.5. `warn()` helper (удалён)

Helper-функция `warn()` была объявлена в `risk_checks.cpp`, но ни разу не вызывалась.

## 9. Runtime-интеграция с pipeline

## 9.1. Создание в `TradingPipeline`

`TradingPipeline` создаёт `ExtendedRiskConfig`, затем вручную маппит в него только часть полей из `config_.risk`.

Дополнительно он подмешивает параметры не из `risk`, а из `trading_params`:

- `max_loss_per_trade_pct`;
- `operational_deadman_ns`;
- `post_loss_cooldown_ns`.

## 9.2. Runtime overrides поверх конфига

После маппинга pipeline дополнительно меняет часть значений:

### Маленький аккаунт

Если `initial_capital < 100`, то:

- `risk_cfg.min_liquidity_depth = 1.0`.

### Futures leverage override

Дальше pipeline принудительно выставляет:

- `max_gross_exposure_pct = max_leverage * 110.0`;
- `max_leverage = futures.max_leverage`;
- `max_symbol_concentration_pct = max_leverage * 110.0`.

Следствие:

- часть risk-лимитов из YAML фактически перезаписывается runtime-логикой pipeline;
- особенно это касается concentration/exposure под плечо USDT-M futures.

## 9.3. Где вызывается `evaluate()`

### Новый entry

Полный pre-trade risk check обязателен для новых позиций:

1. strategy → `TradeIntent`;
2. `execution_alpha`;
3. `portfolio_allocator`;
4. `risk_engine_->evaluate(...)`;
5. только потом execution.

### TWAP-slice

Каждый TWAP-slice тоже проходит через `risk_engine_->evaluate(...)`, но с искусственно собранным `SizingResult`.

Это важно: даже уже спланированная нарезка не считается безусловно безопасной.

## 9.4. Где вызываются stateful hooks

- `record_order_sent()` вызывается после успешной отправки ордера;
- `record_trade_result()` и `record_trade_close()` вызываются при закрытии позиции;
- `set_current_regime()` вызывается при обновлении regime snapshot.

## 9.5. Funding rate в pipeline

Pipeline обновляет `current_funding_rate` через futures query adapter каждые 5 минут.
Теперь rate передаётся в risk engine через `risk_engine_->set_funding_rate(new_rate)`,
и `FundingRateCostCheck` получает живые данные через `RiskContext`.

## 10. Что реально маппится из конфига, а что нет

Из `config.risk` реально загружается и передаётся только ограниченный поднабор полей.

### Публично настраиваемые через YAML

- `max_position_notional`
- `max_daily_loss_pct`
- `max_drawdown_pct`
- `kill_switch_enabled`
- `max_strategy_daily_loss_pct`
- `max_strategy_exposure_pct`
- `max_symbol_concentration_pct`
- `max_same_direction_positions`
- `regime_aware_limits_enabled`
- `stress_regime_scale`
- `trending_regime_scale`
- `chop_regime_scale`
- `max_trades_per_hour`
- `min_trade_interval_sec`
- `max_adverse_excursion_pct`
- `max_realized_daily_loss_pct`
- `utc_cutoff_hour`

### Существуют в `ExtendedRiskConfig`, но не экспонируются через `config.risk`

- `max_slippage_bps`
- `max_orders_per_minute`
- `max_concurrent_positions`
- `max_annual_funding_cost_pct`

Все остальные поля были удалены. Тип конфигурации точно соответствует enforcement layer.

### Существуют и даже хранятся, но в текущем коде не используются проверками

(Нет — все неиспользуемые поля были вычищены.)

Иначе говоря, тип конфигурации теперь точно соответствует реальному enforcement layer.

## 11. `record_trade_close()` и post-trade side effects

Этот метод — важная часть risk-модуля, потому что именно он обновляет state:

- streaks;
- per-symbol / per-strategy pnl;
- turnover timestamps;
- strategy budgets;
- automatic cooldown/day-lock logic.

### Что происходит на loss

- `LossStreakTracker` увеличивает global/symbol/strategy streak;
- `PnlTracker` добавляет realized PnL;
- у strategy budget увеличиваются `daily_loss` и `consecutive_losses`.

### Автоматические lock-и

После `record_trade_close()` могут автоматически ставиться:

1. `Cooldown` по символу после `max_consecutive_losses_per_symbol`;
2. глобальный `Cooldown` после `cooldown_after_n_losses`;
3. `DayLock` после `halt_after_n_losses`.

То есть часть risk-policy логики исполняется не только в `evaluate()`, но и в post-trade hook-ах.

## 12. `evaluate_position()`

Intra-trade мониторинг сейчас очень узкий и использует только две проверки:

1. max adverse excursion;
2. max hold time.

Причём переданный `features` параметр в текущей реализации не используется вообще.

Это значит, что intra-trade risk сейчас:

- не microstructure-aware;
- не spread-aware;
- не uncertainty-aware;
- не funding-aware.

Он ориентирован только на время удержания и PnL damage control.

## 13. Состояние observability

`get_risk_snapshot()` возвращает не полный operational snapshot, а частично заполненный объект.

Сейчас реально заполняются:

- `computed_at`;
- `kill_switch_active`;
- `regime_scaling_factor`;
- `global_state`;
- `active_locks`;
- `current_drawdown_pct`;
- `strategy_budgets`.

Но часть полей остаётся дефолтной:

- `total_risk_utilization`;
- `daily_loss_pct` (явно оставлено `0.0` с комментарием про pipeline);
- `open_positions`;
- `gross_exposure_pct`;
- `rules_triggered`.

То есть observability API существует, но snapshot пока только частично operationally useful.

## 14. Покрытие тестами

У risk-модуля есть отдельный набор `tests/unit/risk/risk_test.cpp`.

По фактическому запуску на момент разбора:

- `36/36` risk-тестов проходят.

Покрыты сценарии:

- базовый approve path;
- kill switch;
- daily loss / realized loss / drawdown;
- max positions / exposure / notional / leverage / slippage;
- order rate / turnover / trade interval;
- stale feed / invalid book / wide spread / low liquidity;
- strategy budget;
- symbol concentration;
- same-direction guard;
- UTC cutoff;
- intra-trade hold / adverse excursion;
- record_trade_close / reset_daily / snapshot / regime scaling;
- funding rate excessive / funding rate normal;
- ReduceSize min-семантика (не компаундится);
- projected post-trade exposure;
- projected post-trade leverage.

## 15. Сильные стороны текущего дизайна

### 15.1. Политики разнесены отдельно от engine orchestration

`IRiskCheck` и `risk_checks.cpp` делают логику расширяемой и удобной для audit/debug.

### 15.2. Есть stateful control plane

Модуль умеет помнить:

- lock-и;
- streak-и;
- turnover;
- strategy budgets;
- drawdown peaks.

Для production risk-layer это правильный класс архитектуры.

### 15.3. Есть интеграция с uncertainty и regime

Risk уже учитывает:

- regime scaling;
- uncertainty cooldown;
- HaltNewEntries;
- uncertainty size multiplier.

### 15.4. Есть отдельный intra-trade API

Модуль отвечает не только за pre-trade допуск, но и за мониторинг открытых позиций.

### 15.5. Есть тестовое покрытие на базовый функционал

34 unit tests дают неплохую защиту от прямых регрессий по основным веткам.

## 16. Оставшиеся ограничения текущей реализации

### 16.1. `PerStrategyRiskCheck` по факту проверяет только daily loss budget

Несмотря на название, strategy exposure и strategy drawdown в текущем коде не контролируются.

### 16.2. `get_risk_snapshot()` возвращает частичный snapshot

Несколько публичных полей DTO остаются незаполненными (daily_loss_pct, gross_exposure_pct, open_positions, rules_triggered).

### 16.3. `evaluate_position()` игнорирует `features`

Intra-trade monitoring пока не использует рынок, а только время и PnL.

## 17. Расхождение с документацией

Файл `docs/risk.md` и код синхронизированы: оба описывают 33 проверки, post-trade проекцию, live funding rate и min-семантику ReduceSize.

## 18. Краткий итог

`risk` в текущем tomorrow-bot — это stateful policy-based risk engine для USDT-M futures, который:

- принимает уже подготовленный `TradeIntent` и аллокированный размер;
- прогоняет их через 33 последовательных policy-checks;
- умеет deny / throttle / reduce size (с min-семантикой);
- проецирует post-trade exposure / leverage / concentration;
- получает live funding rate через `set_funding_rate()`;
- хранит lock-и, streak-и, PnL и drawdown state;
- участвует как в pre-trade, так и в post-trade/intra-trade контуре.

С инженерной точки зрения модуль production-grade:

- есть policy chain с 33 проверками;
- есть stateful risk memory (locks, streaks, pnl, drawdown);
- есть regime/uncertainty integration;
- есть post-trade проекция экспозиции / плеча / концентрации;
- funding rate работает end-to-end;
- ReduceSize использует min-семантику (не компаундится);
- удалён весь spot-артефактный код;
- удалены неиспользуемые конфигурационные поля;
- 36 unit-тестов покрывают основные и edge-case сценарии;
- конфигурационные значения научно обоснованы.

Оставшиеся ограничения:

- `PerStrategyRiskCheck` проверяет только daily loss budget (без exposure/drawdown);
- `get_risk_snapshot()` заполняет не все публичные поля;
- `evaluate_position()` не использует features (только время и PnL).