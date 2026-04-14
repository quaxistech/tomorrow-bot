# Подробный разбор модуля portfolio allocation

Временный аналитический документ.

Источник разбора: текущая реализация `src/portfolio_allocator`, её вызовы из `pipeline`, а также связанные тесты на момент 2026-04-09.
Это описание фактической реализации, а не целевой архитектуры.

## 1. Что входит в модуль

Модуль `portfolio_allocator` состоит из четырёх файлов:

| Файл | Назначение |
|---|---|
| `src/portfolio_allocator/CMakeLists.txt` | Собирает библиотеку `tb_portfolio_allocator` |
| `src/portfolio_allocator/allocation_types.hpp` | Типы входного контекста, exchange filters, audit trail и результата аллокации |
| `src/portfolio_allocator/portfolio_allocator.hpp` | Интерфейс `IPortfolioAllocator` и класс `HierarchicalAllocator` |
| `src/portfolio_allocator/portfolio_allocator.cpp` | Реальная логика sizing, volatility targeting, Kelly scaling, drawdown scaling и exchange filters |

## 2. Главная роль модуля

`portfolio allocation` отвечает за вычисление допустимого размера позиции до передачи сделки в risk engine и execution.

Идея модуля:

1. взять сигнал стратегии;
2. перевести его в quantity/notional;
3. пропустить через бюджетные ограничения;
4. уменьшить размер по неопределённости, волатильности, drawdown и ликвидности;
5. привести размер к биржевым ограничениям;
6. вернуть финальный approved size.

Важное замечание: в production runtime сейчас используется только legacy-путь `compute_size()` + `set_market_context()`. Более богатый `compute_size_v2()` реализован и протестирован, но пока не вызван из рабочих модулей.

## 3. Основные структуры данных

### 3.1. `BudgetHierarchy`

Тип содержит:

- `global_budget`;
- `global_utilization_pct`;
- `regime_budget_pct`;
- `strategy_budget_pct`;
- `symbol_budget_pct`.

Но в текущей реализации реально используется только `global_budget` как fallback и `symbol_budget_pct` в расчёте лимита.

### 3.2. `ExchangeFilters`

Это локальная модель правил биржи:

- `min_quantity`;
- `max_quantity`;
- `quantity_step`;
- `min_notional`;
- `tick_size`;
- `quantity_precision`;
- `price_precision`;
- `taker_fee_pct`;
- `maker_fee_pct`.

На практике `apply_exchange_filters()` использует прежде всего `quantity_step`, `min_quantity`, `max_quantity` и `min_notional`.

### 3.3. `AllocationContext`

Это расширенный контекст для `compute_size_v2()`:

- realized annual volatility;
- regime;
- win rate;
- avg win/loss ratio;
- ADV;
- spread;
- depth;
- current drawdown;
- consecutive losses;
- optional exchange filters;
- `signal_intent`.

### 3.4. `ConstraintDecision`

Аудит одной стадии ограничения:

- имя ограничения;
- лимит;
- входное значение;
- выходное значение;
- был ли constraint binding;
- детали.

### 3.5. `SizingResult`

Результат вычисления размера содержит:

- `approved_quantity`;
- `approved_notional`;
- долю от капитала;
- флаг `was_reduced`;
- причину уменьшения;
- `approved`;
- audit trail ограничений;
- `fee_adjusted_notional`;
- `expected_fee`.

## 4. Интерфейс и реализация

### 4.1. `IPortfolioAllocator`

Интерфейс задаёт три метода:

- `compute_size()` — старый API;
- `compute_size_v2()` — preferred API с полным контекстом;
- `set_market_context()` — stateful способ подать volatility/Kelly/regime контекст перед вызовом legacy API.

### 4.2. `HierarchicalAllocator::Config`

Конфиг делится на блоки:

- лимиты концентрации;
- бюджетная иерархия;
- volatility targeting;
- drawdown scaling;
- liquidity caps.

Особенно важны:

- `max_concentration_pct`;
- `max_strategy_allocation_pct`;
- `target_annual_vol`;
- `max_leverage`;
- `kelly_fraction`;
- `min_size_multiplier` / `max_size_multiplier`;
- drawdown thresholds;
- `max_adv_participation_pct`;
- `max_book_participation_pct`.

## 5. Legacy-путь: `compute_size()`

Это фактический production-путь, который сейчас вызывается из `TradingPipeline`.

### 5.1. Базовая цена

Аллокатор требует `intent.limit_price`. Если её нет или она невалидна, sizing отклоняется.

### 5.2. Исходный объём

Если стратегия уже дала `suggested_quantity`, аллокатор начинает с неё.

Если нет, он строит размер от доступного капитала через helper `compute_effective_available_notional()`.

Формула helper-а:

$$
capital\_base = available\_capital > 0 ? available\_capital : total\_capital
$$

$$
effective\_lev = \max(max\_leverage \cdot 0.5, 1.0)
$$

$$
effective\_available\_notional = capital\_base \cdot effective\_lev
$$

Это консервативная эвристика: legacy sizing использует только половину максимального плеча как базу для предварительной оценки.

### 5.3. Бюджетный лимит

`compute_budget_limit()` считает:

$$
effective\_capital = total\_capital \cdot max\_leverage
$$

$$
budget\_limit = effective\_capital \cdot symbol\_budget\_pct
$$

Если `portfolio.total_capital <= 0`, используется `budget.global_budget` как fallback.

### 5.4. Лимит концентрации

$$
concentration\_limit = total\_capital \cdot max\_leverage \cdot max\_concentration\_pct
$$

### 5.5. Лимит стратегии

Аллокатор суммирует существующий notional всех позиций с тем же `strategy_id` и вычитает его из максимального strategy budget:

$$
strategy\_remaining = total\_capital \cdot max\_leverage \cdot max\_strategy\_allocation\_pct - existing\_strategy\_exposure
$$

### 5.6. Множитель неопределённости

Размер умножается на `clamp(uncertainty_size_multiplier, 0, 1)`.

### 5.7. Volatility targeting и Kelly scaling

Legacy API берёт контекст из полей, ранее установленных `set_market_context()`.

Формула волатильностного множителя:

$$
vol\_ratio = \frac{target\_annual\_vol}{realized\_vol\_annual}
$$

Kelly fraction:

$$
f^* = \frac{p \cdot b - q}{b}
$$

где:

- $p = win\_rate$;
- $q = 1 - p$;
- $b = avg\_win\_loss\_ratio$.

После этого:

$$
kelly\_adj =
\begin{cases}
kelly\_fraction \cdot f^*, & f^* > 0 \\
min\_size\_multiplier, & f^* \le 0
\end{cases}
$$

Итог:

$$
combined = vol\_ratio \cdot \max(kelly\_adj, 0.1) \cdot regime\_multiplier
$$

Дальше множитель ограничивается `max_leverage` и затем зажимается в диапазон `[min_size_multiplier, max_size_multiplier]`.

### 5.8. Regime multiplier

Размер дополнительно масштабируется по режиму рынка:

- сильный тренд: `1.0`;
- слабый тренд: `0.8`;
- mean reversion: `0.7`;
- volatility expansion: `0.5`;
- chop: `0.3`;
- liquidity stress / spread instability: `0.2`;
- anomaly / toxic flow: `0.1`;
- undefined: `0.5`.

### 5.9. Повторное применение жёстких лимитов

После volatility targeting аллокатор ещё раз перепроверяет budget, concentration и strategy caps.

Это нужно потому, что vol multiplier может увеличить размер относительно исходной заявки.

### 5.10. Clamp по доступному капиталу

После предыдущих ограничений legacy API ограничивает размер по `compute_effective_available_notional()`.

### 5.11. Минимальный размер ордера

Legacy API затем принудительно поднимает ордер минимум до `5.0 USDT` с буфером `1.05`.

То есть используется порог:

$$
min\_notional = 5.0 \cdot 1.05
$$

Если капитала на такой минимум нет, sizing отклоняется.

## 6. Расширенный путь: `compute_size_v2()`

Это более зрелый и более детализированный sizing pipeline.

Он делает почти всё, что делает legacy API, но добавляет:

1. `ConstraintDecision` audit trail;
2. drawdown scaling;
3. liquidity caps через ADV и book depth;
4. exchange filters;
5. явный расчёт expected fee;
6. `fee_adjusted_notional`.

### 6.1. Drawdown scaling

Размер снижается линейно между двумя порогами:

- `drawdown_scale_start_pct`;
- `drawdown_scale_max_pct`.

При достижении максимального порога размер не обнуляется полностью, а уменьшается до `drawdown_min_size_fraction`.

### 6.2. Liquidity cap

Лимит определяется минимумом из:

- `ADV * max_adv_participation_pct`;
- `book_depth_notional * max_book_participation_pct`.

### 6.3. Exchange filters

`apply_exchange_filters()`:

- округляет quantity вниз до `quantity_step`;
- проверяет `min_quantity` и при возможности поднимает до минимума;
- ограничивает `max_quantity`;
- проверяет `min_notional`;
- пишет отдельную запись в audit trail.

### 6.4. Fee-aware result

На выходе v2 считает:

$$
expected\_fee = approved\_notional \cdot taker\_fee\_pct
$$

$$
fee\_adjusted\_notional = approved\_notional + expected\_fee
$$

## 7. Как модуль используется в runtime

### 7.1. Что реально используется

В `TradingPipeline` сейчас используется именно такой путь:

1. перед sizing вызывается `portfolio_allocator_->set_market_context(...)`;
2. затем вызывается `portfolio_allocator_->compute_size(intent, portfolio_snapshot, combined_size_mult)`.

### 7.2. Что пока не используется

В кодовой базе нет production-вызовов `compute_size_v2()`.

Это означает, что в live-пути сейчас не задействованы:

- constraint audit;
- drawdown scaling из `AllocationContext`;
- liquidity cap по ADV/depth;
- exchange filters внутри allocator;
- fee-adjusted sizing result.

## 8. Покрытие тестами

У модуля есть отдельный набор unit-тестов:

- legacy `compute_size()`;
- `compute_size_v2()`;
- exchange filter utilities;
- volatility targeting через `set_market_context()`.

На момент анализа профильные 16 allocator-тестов проходят.

## 9. Сильные стороны текущего дизайна

### 9.1. Размер зависит не только от капитала

Модуль учитывает volatility, Kelly edge и regime, а не просто выделяет фиксированный процент капитала.

### 9.2. Есть многоуровневые caps

Budget, concentration и strategy limits накладываются последовательно, что делает sizing defensive.

### 9.3. `compute_size_v2()` уже близок к production-grade API

В нём есть constraint audit, liquidity-aware sizing и exchange-aware post-processing.

### 9.4. Аллокатор отделён от risk engine

Он отвечает именно за candidate size, а не за окончательное торговое разрешение.

## 10. Главные ограничения текущей реализации

### 10.1. `compute_size_v2()` пока не подключён в production runtime

Архитектурно он выглядит как preferred API, но фактический live-путь использует legacy `compute_size()`.

### 10.2. `BudgetHierarchy` реализован частично

Поля:

- `global_utilization_pct`;
- `regime_budget_pct`;
- `strategy_budget_pct`

существуют как типы, но не участвуют в расчётах.

### 10.3. Разные минимальные пороги ордера в legacy и v2

Legacy API использует минимум около `5 USDT`, а `compute_size_v2()` в fallback-ветке использует `1.10 USDT` или exchange filters.

Это означает, что два API могут выдавать разный результат на одинаковом входе.

### 10.4. Legacy API не использует exchange filters

Production sizing сейчас не приводит quantity к `quantity_step` и не учитывает `min_notional` средствами allocator-а. Эта задача пока смещена в другие слои исполнения.

### 10.5. `AllocationContext.signal_intent` сейчас не участвует в формуле

Поле есть в модели, но в текущей реализации никак не влияет на sizing.

### 10.6. Комментарии в `ExchangeFilters` не полностью синхронизированы с константами комиссий

Типы ссылаются на futures fee constants, но текстовые комментарии рядом выглядят историческими и не полностью совпадают с фактическими значениями Bitget USDT-M в `common/constants.hpp`.

## 11. Практический смысл для текущего бота

В текущем виде модуль даёт working production sizing для скальпинга на USDT-M, потому что:

- ограничивает концентрацию;
- учитывает плечо;
- адаптирует размер к волатильности;
- снижает размер в плохих режимах;
- не позволяет ордеру уйти за предел доступного капитала.

Но реальная production-логика пока использует упрощённую ветку allocator-а. Более полный контекстный sizing уже написан, покрыт тестами, но ещё не интегрирован в live pipeline.

## 12. Краткий итог

`portfolio allocator` — это модуль sizing-а, а не окончательного risk approval.

Его текущая production-роль: дать безопасный предварительный размер позиции для USDT-M фьючерсного входа с учётом капитала, плеча, режима рынка, Kelly edge и базовых бюджетных ограничений.

Его стратегический потенциал больше текущего runtime-использования: `compute_size_v2()` уже содержит более зрелую модель с audit trail, liquidity constraints и exchange filters, но пока остаётся подготовленной, а не реально задействованной веткой.