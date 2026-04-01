# Риск-менеджмент

## Обзор

`ProductionRiskEngine` — policy-based движок, выполняющий **33 последовательные проверки**
перед каждым ордером. Любая проверка может отклонить ордер.

```cpp
RiskDecision evaluate(
    const TradeIntent& intent,
    const SizingResult& sizing,
    const PortfolioSnapshot& portfolio,
    const FeatureSnapshot& features,
    const ExecutionAlphaResult& exec_alpha,
    const UncertaintySnapshot& uncertainty
);
```

---

## 33 проверки

| # | Проверка | Описание |
|---|---------|----------|
| 1 | **KillSwitchCheck** | Полная блокировка торговли при активном kill switch |
| 2 | **DayLockCheck** | Блокировка торговли на весь день (после критических событий) |
| 3 | **SymbolLockCheck** | Блокировка конкретного символа |
| 4 | **StrategyLockCheck** | Блокировка конкретной стратегии |
| 5 | **SymbolCooldownCheck** | Пауза после недавней сделки по символу |
| 6 | **DailyLossCheck** | Суммарный убыток за день (unrealized + realized) |
| 7 | **RealizedDailyLossCheck** | Реализованный дневной убыток |
| 8 | **MaxDrawdownCheck** | Максимальная просадка от пика |
| 9 | **MaxPositionsCheck** | Лимит одновременно открытых позиций |
| 10 | **SameDirectionCheck** | Лимит позиций в одном направлении (long или short) |
| 11 | **ExposureLimitCheck** | Макс. суммарная экспозиция (нотионал) |
| 12 | **PerTradeRiskCheck** | Макс. риск на одну сделку |
| 13 | **MaxLeverageCheck** | Проверка кредитного плеча |
| 14 | **MaxSlippageCheck** | Допустимый slippage |
| 15 | **ConsecutiveLossesCheck** | Серия убыточных сделок подряд |
| 16 | **PerSymbolRiskCheck** | Концентрация риска на символ |
| 17 | **PerStrategyRiskCheck** | Бюджет стратегии (дневной убыток, экспозиция) |
| 18 | **OrderRateCheck** | Частота ордеров (макс. в час) |
| 19 | **TurnoverRateCheck** | Оборачиваемость портфеля |
| 20 | **TradeIntervalCheck** | Мин. интервал между сделками по символу |
| 21 | **StaleFeedCheck** | Актуальность рыночных данных |
| 22 | **BookQualityCheck** | Качество ордербука (глубина, актуальность) |
| 23 | **SpreadCheck** | Допустимый спред |
| 24 | **LiquidityCheck** | Достаточная ликвидность |
| 25 | **MaxLossPerTradeCheck** | Абсолютный лимит убытка на сделку |
| 26 | **UtcCutoffCheck** | Остановка торговли в заданный час UTC |
| 27 | **RegimeScaledLimitsCheck** | Динамическое масштабирование лимитов по режиму |
| 28 | **UncertaintyLimitsCheck** | Блокировка при высокой неопределённости |
| 29 | **UncertaintyCooldownCheck** | Cooldown после всплеска неопределённости |
| 30 | **UncertaintyExecutionModeCheck** | Ограничение execution mode при uncertainty |
| 31 | **SpotSellCheck** | Запрет SELL без открытой long-позиции (spot-режим) |
| 32 | **IntradayDrawdownCheck** | Внутридневная просадка |
| 33 | **DrawdownHardStopCheck** | Жёсткий лимит просадки (полная остановка) |

---

## Kill Switch

Механизм аварийной остановки:

- Активируется при критическом нарушении (hard drawdown, exchange failure и т.д.)
- Broadcast через Supervisor → все pipeline немедленно останавливаются
- Может быть активирован программно (`activate_kill_switch()`) или через конфиг
- Production-режим **требует** `kill_switch_enabled: true`

---

## Режимное масштабирование

При `regime_aware_limits_enabled: true` лимиты масштабируются:

| Режим | Множитель |
|-------|-----------|
| Trending | `trending_regime_scale` (пр.: 1.2) |
| Stress | `stress_regime_scale` (пр.: 0.5) |
| Chop | `chop_regime_scale` (пр.: 0.7) |
| Остальные | 1.0 |

Затрагивает: max_positions, exposure_limit, trade_rate.

---

## Uncertainty-aware проверки

Три проверки, интегрированные с UncertaintyEngine:

1. **UncertaintyLimitsCheck** — блокировка при aggregate uncertainty > порога
2. **UncertaintyCooldownCheck** — принудительная пауза после всплеска
3. **UncertaintyExecutionModeCheck** — ограничение до passive execution при высокой uncertainty

---

## Дополнительные методы

| Метод | Назначение |
|-------|------------|
| `record_order_sent()` | Обновление rate-лимитов |
| `record_trade_result()` | Учёт PnL для дневных лимитов |
| `record_trade_close()` | Учёт закрытия позиции |
| `evaluate_position()` | Проверка открытой позиции (hold time, adverse excursion) |
| `set_current_regime()` | Обновление текущего режима для scaled limits |
| `reset_daily()` | Сброс дневных счётчиков |

---

## Конфигурация

Все пороги настраиваются в секции `risk:` конфига. Подробнее: [config.md](config.md#risk).
