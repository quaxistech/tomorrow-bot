# Правила Риск-движка — Справочник

## Обзор

`ProductionRiskEngine` — desk-grade риск-движок с **27 правилами pre-trade контроля**,
**intra-trade мониторингом** открытых позиций и **post-trade учётом** per-strategy budgets.

Каждое нарушение добавляет `RiskReasonCode` в решение.
Вердикт: `Approved`, `Denied`, `ReduceSize` или `Throttled`.

### Архитектура риск-фаз

| Фаза | Метод | Описание |
|------|-------|----------|
| **PreTrade** | `evaluate()` | 27 проверок перед отправкой ордера |
| **IntraTrade** | `evaluate_position()` | Мониторинг MAE и времени удержания |
| **PostTrade** | `record_trade_close()` | Обновление per-strategy budgets и turnover |

## Таблица правил (Pre-Trade)

| # | Код | Описание | Порог (по умолчанию) | Вердикт |
|---|-----|----------|---------------------|---------|
| 1 | `KILL_SWITCH` | Аварийный выключатель активирован | N/A | Denied |
| 2 | `MAX_DAILY_LOSS` | Дневной убыток (realized+unrealized) | 2% капитала | Denied |
| 3 | `MAX_DRAWDOWN` | Просадка от пика | 5% капитала | Denied |
| 4 | `MAX_POSITIONS` | Достигнут макс позиций | 5 позиций | Denied |
| 5 | `MAX_EXPOSURE` | Валовая экспозиция | 50% капитала | Denied |
| 6 | `MAX_NOTIONAL` | Номинал позиции | $10,000 | ReduceSize |
| 7 | `MAX_LEVERAGE` | Плечо | 3.0x | Denied |
| 8 | `MAX_SLIPPAGE` | Ожидаемое проскальзывание | 30 бп | Denied |
| 9 | `ORDER_RATE` | Частота ордеров | 10/мин | Throttled |
| 10 | `CONSECUTIVE_LOSSES` | Серия убытков подряд | 5 подряд | Denied |
| 11 | `STALE_FEED` | Данные рынка устарели | 5 сек | Denied |
| 12 | `INVALID_BOOK` | Стакан невалиден | BookQuality::Valid | Denied |
| 13 | `WIDE_SPREAD` | Спред слишком широкий | 50 бп | Denied |
| 14 | `LOW_LIQUIDITY` | Недостаточная ликвидность | 100 единиц | Denied |
| 15 | `MAX_LOSS_PER_TRADE` | Позиция в убытке > лимита | 1% капитала | Denied |
| 16 | `STRATEGY_BUDGET` | Дневной убыток стратегии | 1.5% капитала | Denied |
| 17 | `SYMBOL_CONCENTRATION` | Доля капитала на один символ | 35% капитала | Denied |
| 18 | `SAME_DIRECTION` | Позиции в одном направлении | 3 позиции | Denied |
| 19 | `UTC_CUTOFF` | Торговля после cutoff часа UTC | -1 (отключено) | Denied |
| 20 | `TURNOVER_RATE` | Превышен лимит сделок в час | 8/час | Throttled |
| 21 | `REALIZED_DAILY_LOSS` | Реализованный дневной убыток | 1.5% капитала | Denied |
| 22 | `TRADE_INTERVAL` | Слишком частые сделки по символу | 30 сек | Throttled |
| 23 | `REGIME_SCALED` | Масштабирование лимитов по режиму | regime_aware | ReduceSize |
| 24 | `UNCERTAINTY_LIMITS` | Uncertainty превышает порог | 0.8 | Denied |
| 25 | `UNCERTAINTY_COOLDOWN` | Uncertainty cooldown активен | 60 сек | Throttled |
| 26 | `GLOBAL_POSITION_LIMITS` | Глобальные лимиты позиций (v0.4, Supervisor) | max_positions | Denied |
| 27 | `SPOT_SELL_WITHOUT_POSITION` | SELL без открытой long-позиции (v0.5) | has_position | Denied |

## Intra-Trade мониторинг

| Проверка | Описание | Порог (по умолчанию) | Действие |
|----------|----------|---------------------|----------|
| `MAX_ADVERSE_EXCURSION` | Неблагоприятное отклонение | 3% капитала | should_close |
| `MAX_HOLD_TIME` | Время удержания позиции | 1 час (3600с) | should_close |

Вызывается через `evaluate_position()` для каждой открытой позиции.
Возвращает `IntraTradeAssessment` с рекомендацией закрытия или уменьшения.

## Regime-Aware Scaling

При включении `regime_aware_limits_enabled`, движок масштабирует лимиты по режиму рынка:

| Режим | Множитель (по умолчанию) | Примеры |
|-------|-------------------------|---------|
| Стресс | 0.5x | LiquidityStress, VolatilityExpansion, AnomalyEvent, ToxicFlow |
| Тренд | 1.2x | StrongUptrend, StrongDowntrend |
| Боковик | 0.7x | Chop, LowVolCompression |
| Нормальный | 1.0x | Все остальные |

Режим устанавливается через `set_current_regime()` на каждом цикле pipeline.

## Per-Strategy Budgets

Каждая стратегия имеет независимый бюджет риска, отслеживаемый через `StrategyRiskBudget`:

- **daily_loss** — реализованный убыток за день (USD)
- **trades_today** — количество сделок за день
- **consecutive_losses** — серия убытков подряд

Обновляется через `record_trade_close()`. Сбрасывается через `reset_daily()`.

## Observability

`get_risk_snapshot()` возвращает `RiskSnapshot` с:
- `total_risk_utilization` — общая утилизация лимитов [0,1]
- `kill_switch_active` — состояние аварийного выключателя
- `regime_scaling_factor` — текущий множитель режима
- `strategy_budgets` — бюджеты всех стратегий

## Детали правил

### 1. KILL_SWITCH
- **Описание**: Аварийный выключатель — немедленная блокировка всех ордеров
- **Активация**: `activate_kill_switch(reason)` — вручную или автоматически
- **Деактивация**: `deactivate_kill_switch()` — только вручную
- **Серьёзность**: 1.0 (абсолютная)

### 2. MAX_DAILY_LOSS
- **Формула**: `|min(total_pnl, 0)| / total_capital * 100`
- **Поведение**: Блокирует все новые ордера до конца торгового дня
- **Сброс**: При вызове `PortfolioEngine::reset_daily()`

### 3. MAX_DRAWDOWN
- **Формула**: `(peak_equity - current_equity) / peak_equity * 100`
- **Пик**: Отслеживается автоматически при обновлении цен
- **Поведение**: Блокирует при превышении — требует ручного вмешательства

### 4. MAX_POSITIONS
- **Проверка**: `open_positions_count >= max_concurrent_positions`
- **Поведение**: Блокирует открытие новых позиций

### 5. MAX_EXPOSURE
- **Формула**: `gross_exposure / total_capital * 100`
- **Gross**: Сумма модулей всех позиций (long + short)

### 6. MAX_NOTIONAL
- **Особенность**: Единственное правило, которое может дать ReduceSize
- **Поведение**: Уменьшает объём пропорционально (`max_notional / actual_notional`)

### 7. MAX_LEVERAGE
- **Формула**: `gross_exposure / total_capital`
- **Поведение**: Блокирует при превышении максимального плеча

### 8. MAX_SLIPPAGE
- **Источник**: `ExecutionAlphaResult.quality.estimated_slippage_bps`
- **Поведение**: Отклоняет, если ожидаемое проскальзывание слишком велико

### 9. ORDER_RATE
- **Механизм**: Скользящее окно 60 секунд
- **Вердикт**: Throttled (повторить позже)
- **Регистрация**: `record_order_sent()` после каждой отправки

### 10. CONSECUTIVE_LOSSES
- **Механизм**: Счётчик убыточных сделок подряд из PortfolioSnapshot
- **Сброс**: При прибыльной сделке или `reset_daily()`

### 11. STALE_FEED
- **Проверка**: `is_feed_fresh` флаг + `market_data_age_ns > max_feed_age_ns`
- **Цель**: Защита от торговли на устаревших данных

### 12. INVALID_BOOK
- **Проверка**: `book_quality != BookQuality::Valid`
- **Значения**: Stale, Desynced, Resyncing, Uninitialized → отклонение

### 13. WIDE_SPREAD
- **Проверка**: `spread_bps > max_spread_bps`
- **Цель**: Защита от торговли при неликвидном рынке

### 14. LOW_LIQUIDITY
- **Проверка**: `bid_depth_5_notional + ask_depth_5_notional < min_liquidity_depth`
- **Цель**: Защита от значительного market impact

### 15. MAX_LOSS_PER_TRADE
- **Проверка**: Если позиция теряет > X% капитала, блокируем новые ордера
- **Цель**: Предотвращение накопления убытков при открытии новых позиций

### 16. STRATEGY_BUDGET
- **Проверка**: `strategy_daily_loss / total_capital * 100 >= max_strategy_daily_loss_pct`
- **Цель**: Изоляция убытков по стратегиям — одна стратегия не съедает весь бюджет

### 17. SYMBOL_CONCENTRATION
- **Проверка**: `symbol_exposure / total_capital * 100 >= max_symbol_concentration_pct`
- **Цель**: Диверсификация — предотвращение чрезмерной концентрации на одном активе

### 18. SAME_DIRECTION
- **Проверка**: Количество позиций в одном направлении >= лимита
- **Цель**: Контроль кластерного риска — защита от однонаправленной экспозиции

### 19. UTC_CUTOFF
- **Проверка**: Текущий час UTC >= utc_cutoff_hour
- **Цель**: Прекращение торговли в заданное время (для дневного окна)

### 20. TURNOVER_RATE
- **Механизм**: Скользящее окно 1 час, подсчёт закрытых сделок
- **Вердикт**: Throttled — предотвращение чрезмерного churning

### 21. REALIZED_DAILY_LOSS
- **Проверка**: `|min(realized_pnl_today, 0)| / total_capital * 100`
- **Отличие от #2**: Учитывает только реализованные убытки (не unrealized)

### 22. TRADE_INTERVAL
- **Механизм**: Минимальный интервал между сделками одного символа
- **Вердикт**: Throttled — защита от перебора рынка

### 23. REGIME_SCALED_LIMITS
- **Механизм**: Записывает множитель режима в RiskDecision
- **Цель**: Адаптация рисковых лимитов к текущим рыночным условиям

## Конфигурация

Все пороги настраиваются через YAML секцию `risk:` и маппятся в `ExtendedRiskConfig`.
Определение: `src/risk/risk_types.hpp`, `src/config/config_types.hpp`.

### Пример конфигурации (paper.yaml)

```yaml
risk:
  max_position_notional: 100.0
  max_daily_loss_pct: 2.0
  max_drawdown_pct: 5.0
  kill_switch_enabled: true
  max_strategy_daily_loss_pct: 1.5
  max_strategy_exposure_pct: 30.0
  max_symbol_concentration_pct: 35.0
  max_same_direction_positions: 3
  regime_aware_limits_enabled: true
  stress_regime_scale: 0.5
  trending_regime_scale: 1.2
  chop_regime_scale: 0.7
  max_trades_per_hour: 8
  min_trade_interval_sec: 30.0
  max_adverse_excursion_pct: 3.0
  max_realized_daily_loss_pct: 1.5
  utc_cutoff_hour: -1
```

## Тестовое покрытие

31 тестов в `tests/unit/risk/risk_test.cpp`:
- 18 тестов для правил 1–15 + multiple violations + to_string + kill switch deactivation
- 13 тестов для правил 16–23 + intra-trade + record_trade_close + set_regime + reset_daily + snapshot
