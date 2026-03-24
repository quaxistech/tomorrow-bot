# Правила Риск-движка — Справочник

## Обзор

`ProductionRiskEngine` проверяет **14 правил** последовательно.
Каждое нарушение добавляет `RiskReasonCode` в решение.
Вердикт: `Approved`, `Denied`, `ReduceSize` или `Throttled`.

## Таблица правил

| # | Код | Описание | Порог (по умолчанию) | Вердикт |
|---|-----|----------|---------------------|---------|
| 1 | `KILL_SWITCH` | Аварийный выключатель активирован | N/A | Denied |
| 2 | `MAX_DAILY_LOSS` | Дневной убыток превышает лимит | 2% капитала | Denied |
| 3 | `MAX_DRAWDOWN` | Просадка от пика превышает лимит | 5% капитала | Denied |
| 4 | `MAX_POSITIONS` | Достигнут макс. числа позиций | 5 позиций | Denied |
| 5 | `MAX_EXPOSURE` | Валовая экспозиция превышает лимит | 50% капитала | Denied |
| 6 | `MAX_NOTIONAL` | Номинал позиции превышает лимит | $10,000 | ReduceSize |
| 7 | `MAX_LEVERAGE` | Плечо превышает лимит | 3.0x | Denied |
| 8 | `MAX_SLIPPAGE` | Проскальзывание превышает лимит | 30 бп | Denied |
| 9 | `ORDER_RATE` | Частота ордеров превышает лимит | 10/мин | Throttled |
| 10 | `CONSECUTIVE_LOSSES` | Серия убытков достигла лимита | 5 подряд | Denied |
| 11 | `STALE_FEED` | Данные рынка устарели | 5 сек | Denied |
| 12 | `INVALID_BOOK` | Стакан ордеров невалиден | BookQuality::Valid | Denied |
| 13 | `WIDE_SPREAD` | Спред слишком широкий | 50 бп | Denied |
| 14 | `LOW_LIQUIDITY` | Недостаточная ликвидность | 100 единиц | Denied |

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
- **Механизм**: Счётчик убыточных сделок подряд
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

## Конфигурация

Все пороги настраиваются через `ExtendedRiskConfig`.
Определение: `src/risk/risk_types.hpp`.
