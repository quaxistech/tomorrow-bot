# Подробный разбор модуля leverage

Временный аналитический документ.

Источник разбора: текущая реализация `src/leverage`, реальные вызовы из `pipeline`,
загрузка и валидация `futures`-конфига, unit-тесты `tests/unit/leverage`, а также
проверочная заметка по математике движка из repository memory.
Это описание фактической реализации на момент 2026-04-09 (после production-grade ревизии).

## 1. Что входит в модуль

Модуль `leverage` очень компактный и состоит из трёх файлов:

| Файл | Назначение |
|---|---|
| `src/leverage/CMakeLists.txt` | Сборка статической библиотеки `tb_leverage` |
| `src/leverage/leverage_engine.hpp` | Публичные типы и интерфейс concrete-движка |
| `src/leverage/leverage_engine.cpp` | Реальная логика расчёта плеча, ликвидации и safety-check |

Зависимости по CMake:

- `tb_common`
- `tb_config`
- `tb_logging`

Архитектурно это не policy-chain и не state-machine, как `risk`, а один компактный
stateful calculator, который получает рыночный контекст и возвращает решение по плечу.

## 2. Главная роль модуля

`LeverageEngine` отвечает за три задачи:

1. вычислить адаптивное кредитное плечо для нового фьючерсного входа;
2. оценить цену ликвидации для выбранного плеча;
3. заблокировать вход, если буфер до ликвидации слишком мал.

Практически он стоит между:

- `regime`;
- `uncertainty`;
- `world_model` / adversarial signals;
- `portfolio`;
- `pipeline`;
- `execution` / futures submitter.

Это pre-trade guard для USDT-M фьючерсной торговли, который влияет и на
допустимое плечо, и на само решение отправлять новый ордер.

## 3. Публичный контракт модуля

### 3.1. `LeverageDecision`

Главный результат расчёта — `LeverageDecision`.

Он содержит:

- `leverage` — финальное целое плечо после всех множителей, Kelly cap и EMA;
- `liquidation_price` — расчётная цена ликвидации;
- `liquidation_buffer_pct` — расстояние до ликвидации в процентах от текущей цены;
- `is_safe` — хватает ли буфера до ликвидации;
- `rationale` — человекочитаемая строка с разложением всех множителей.

Также внутри решения сохраняются audit-компоненты расчёта:

- `base_leverage`;
- `volatility_factor`;
- `drawdown_factor`;
- `conviction_factor`;
- `funding_factor`;
- `adversarial_factor`;
- `uncertainty_factor`.

### 3.2. `LeverageContext`

`LeverageContext` — агрегированный вход вместо 9 отдельных параметров:

- `RegimeLabel regime`;
- `UncertaintyLevel uncertainty`;
- `double atr_normalized` — ATR / price (калибровка: 1-min свечи);
- `double drawdown_pct`;
- `double adversarial_severity` — `[0..1]`;
- `double conviction` — `[0..1]`;
- `double funding_rate` — Bitget 8-hour rate (decimal: 0.0001 = 0.01%);
- `PositionSide position_side`;
- `double entry_price`.

### 3.3. `LiquidationParams`

DTO для статической функции расчёта ликвидации:

- `entry_price`;
- `position_side`;
- `leverage`;
- `maintenance_margin_rate` (default 0.4%);
- `taker_fee_rate` (default 0.06% — Bitget futures taker).

### 3.4. `LeverageEngine`

Публичный API:

- `compute_leverage(...)` — основная точка входа в расчёт плеча;
- `compute_leverage(const LeverageContext&)` — перегрузка с контекстом;
- `update_edge_stats(win_rate, win_loss_ratio)` — обновление Kelly-статистики;
- `compute_liquidation_price(...)` — статический расчёт цены ликвидации;
- `is_liquidation_safe(...)` — статическая проверка safety по буферу;
- `update_config(...)` — hot reload конфига.

## 4. Внутреннее состояние

Движок **stateful**:

- `config_` — активный `FuturesConfig`;
- `mutex_` — защита от гонок между `compute_leverage`, `update_edge_stats`, `update_config`;
- `win_rate_` и `win_loss_ratio_` — rolling edge stats для Kelly cap;
- `ema_leverage_` и `ema_initialized_` — состояние EMA-сглаживания.

`compute_leverage()` не является pure function — EMA зависит от истории.

## 5. Как работает `compute_leverage()`

### Шаг 1. Блокировка mutex

Весь расчёт под `std::lock_guard<std::mutex>`.

### Шаг 2. Базовое плечо по режиму

- `Trending` → `config_.leverage_trending`
- `Ranging` → `config_.leverage_ranging`
- `Volatile` → `config_.leverage_volatile`
- `Unclear` → `config_.default_leverage`

### Шаг 3. Шесть множителей

- `volatility_factor`
- `drawdown_factor`
- `conviction_factor`
- `funding_factor`
- `adversarial_factor`
- `uncertainty_factor`

### Шаг 4. Композитный raw leverage

$$
raw = base\_leverage
\cdot vol
\cdot dd
\cdot conv
\cdot fund
\cdot adv
\cdot unc
$$

### Шаг 5. Kelly Criterion upper bound

Формула полного Kelly:

$$
f^* = \frac{b p - q}{b}, \quad q = 1 - p
$$

Если $f^* \le 0$, возвращается `min_leverage` (отрицательный edge).

Half-Kelly (Thorp, 2006): $f = f^*/2$, верхний лимит: $kelly\_cap = 1/f$.

Дефолтные edge stats: `win_rate_ = 0.50`, `win_loss_ratio_ = 1.5` → Kelly cap ≈ 12x.

### Шаг 6. EMA smoothing (Brown, 1956)

$$
ema_t = \alpha \cdot raw_t + (1 - \alpha) \cdot ema_{t-1}
$$

`alpha = config_.leverage_engine.ema_alpha` (default 0.3, ~5-tick lookback).

### Шаг 7. Округление и clamp

$$
[\max(1, min\_leverage), max\_leverage]
$$

### Шаг 8. Ликвидационная цена (если entry_price > 0)

### Шаг 9. Safety-check по буферу

### Шаг 10. Формирование `rationale`

## 6. Все компоненты расчёта

### 6.1. Базовое плечо по режиму

4 режима → 4 config-значения (Trending, Ranging, Volatile, Unclear).

### 6.2. Волатильность: `volatility_multiplier()`

Вход — `atr_normalized = ATR / price` (откалиброван для 1-min свечей).

Piecewise-linear:

| ATR/price | Множитель | Обоснование |
|---|---|---|
| ≤ vol_low (0.1%) | 1.0 | Calm market (BTC 1-min typical) |
| → vol_mid (0.3%) | → 0.7 | Moderate vol (altcoin normal) |
| → vol_high (0.8%) | → 0.4 | Significant intraday vol |
| → vol_extreme (2.0%) | → 0.2 | Flash-crash territory |
| > vol_extreme | → vol_floor (0.10) | Regime-shift protection |

Научное обоснование: Parkinson (1980) high-low vol estimator; эмпирическая
калибровка на BTC/ETH/SOL 1-min ATR/price данных.

### 6.3. Просадка: `drawdown_multiplier()` (Grossman & Zhou, 1993)

$$
mult = floor + (1 - floor) \cdot \left(1 - \tanh(k \cdot dd / dd_{half})\right)
$$

- `floor = drawdown_floor_mult` (default 0.10)
- `dd_half = drawdown_halfpoint_pct` (default 10.0%)
- `k = 0.5493 ≈ atanh(0.5)`

C∞-гладкая кривая: при 10% DD → множитель ≈ 0.55; при 50% DD → ≈ 0.11.

### 6.4. Conviction: `conviction_multiplier()`

Двухсегментная кривая с настраиваемым floor и ceiling:

- `[0, breakpoint]` — рост от `conviction_min_mult` (0.40) до `1.00`
- `[breakpoint, 1]` — рост от `1.00` до `conviction_max_mult` (1.30)

### 6.5. Funding: `funding_multiplier()`

Учитывается **только если текущая сторона позиции платит funding**:

- Long + funding_rate > 0 → платим → штраф
- Short + funding_rate < 0 → платим → штраф

Если `|rate| < funding_rate_threshold`, множитель = 1.0.

Экспоненциальное затухание:

$$
excess = \frac{|rate| - threshold}{threshold}, \quad
k = -\ln(1 - penalty), \quad
mult = e^{-k \cdot excess}
$$

Floor: 0.15.

Threshold откалиброван на Bitget 8-hour funding rate (decimal):
- Default: 0.0005 (0.05% per 8h) — elevated funding.
- При 20x leverage: стоимость удержания ≈ 1% margin per 8h.

### 6.6. Adversarial severity: `adversarial_multiplier()`

$$
mult = lerp(1.0, 0.15, severity)
$$

### 6.7. Uncertainty: `uncertainty_multiplier()`

| Level | Множитель |
|---|---|
| Low | 1.00 |
| Moderate | 0.80 |
| High | 0.55 |
| Extreme | 0.25 |

## 7. Ликвидационная цена и safety

### 7.1. `compute_liquidation_price()` (Bitget USDT-M isolated margin)

Long:

$$
liq = entry \cdot (1 - 1 / leverage + mmr + fee)
$$

Short:

$$
liq = entry \cdot (1 + 1 / leverage - mmr - fee)
$$

Где:
- `mmr = maintenance_margin_rate` (config, default 0.4%)
- `fee = taker_fee_rate` (config, default 0.06% — Bitget futures taker)

Оба значения берутся из конфига, не захардкожены.

### 7.2. `is_liquidation_safe()`

Для Long: $buffer = (current - liq) / current \cdot 100$

Для Short: $buffer = (liq - current) / current \cdot 100$

Безопасно, если $buffer \ge min\_buffer\_pct$.

## 8. Реальная интеграция с pipeline

### 8.1. Создание движка

`TradingPipeline` создаёт `LeverageEngine` в конструкторе на основе `config_.futures`.

### 8.2. Где используется

Leverage применяется в блоке новых входов, после opportunity_cost, portfolio_allocator, risk_engine.
Это поздний pre-execution guard.

### 8.3. LeverageContext assembly

Pipeline собирает контекст:

- `regime = regime.label`
- `uncertainty = uncertainty.level`
- `atr_normalized = atr_14 / mid_price`
- `drawdown_pct = portfolio_->snapshot().pnl.current_drawdown_pct`
- `conviction = intent.conviction`
- `funding_rate = current_funding_rate_` (обновляется каждые 5 мин через Bitget API)
- `position_side = intent.position_side`
- `entry_price = snapshot.mid_price`

### 8.4. Adversarial severity (собирается в pipeline)

Максимум нескольких источников: world state, spread, book instability, VPIN, aggressive flow,
ML fingerprint edge + multi-trigger бонус.

### 8.5. После `compute_leverage()`

1. Если `!is_safe` → сделка отклоняется
2. Плечо синхронизируется с биржей (с debounce: `min_leverage_change_delta`)
3. Плечо прокидывается в execution engine

### 8.6. Kelly-статистика

Обновляется после закрытия позиции: `update_edge_stats(rolling_win_rate, rolling_win_loss_ratio)`.

## 9. Конфигурация

### 9.1. FuturesConfig (используемые поля)

- `default_leverage`, `max_leverage`, `min_leverage`
- `leverage_trending`, `leverage_ranging`, `leverage_volatile`
- `liquidation_buffer_pct`
- `funding_rate_threshold` (0.0005 = 0.05% per 8h)
- `funding_rate_penalty`
- `maintenance_margin_rate`
- `leverage_engine.*`

### 9.2. LeverageEngineConfig

- `vol_low_atr` (0.001), `vol_mid_atr` (0.003), `vol_high_atr` (0.008), `vol_extreme_atr` (0.02), `vol_floor` (0.10)
- `conviction_min_mult` (0.40), `conviction_breakpoint` (0.70), `conviction_max_mult` (1.30)
- `drawdown_floor_mult` (0.10), `drawdown_halfpoint_pct` (10.0)
- `ema_alpha` (0.3)
- `taker_fee_rate` (0.0006) — используется в формуле ликвидации
- `min_leverage_change_delta` (2) — debounce в pipeline

### 9.3. Валидация

ConfigValidator проверяет:
- Leverage ranges [1, 125], взаимную согласованность min ≤ default ≤ max
- Режимные плечи ∈ [1, max_leverage]
- Liquidation buffer ∈ (0, 50]
- Funding threshold ∈ (0, 0.1], penalty ∈ [0, 1]
- Maintenance margin rate ∈ (0, 0.1]
- Vol breakpoints: строго упорядочены (low < mid < high < extreme)
- Vol floor ∈ (0, 1]
- Conviction: min_mult ∈ [0, 1], breakpoint ∈ (0, 1), max_mult ∈ [1, 3]
- Drawdown: floor ∈ [0, 1], halfpoint > 0
- EMA alpha ∈ (0, 1]
- Taker fee rate ∈ [0, 0.01]

## 10. Тестовое покрытие

13/13 leverage-тестов проходят:

- Базовое плечо по режиму
- Volatility multiplier (5 уровней + плавность)
- Drawdown multiplier (0%, 5%, 10%, 40%)
- Conviction multiplier (0, 0.5, 0.7, 0.9, 1.0)
- Funding multiplier (нулевой, ниже порога, paying/receiving, long/short)
- Adversarial multiplier (0, 0.5, 1.0 + плавность)
- Uncertainty multiplier (Low, Moderate, Extreme)
- Clamp результата (min, max)
- Liquidation price (Long, Short, leverage=1, invalid)
- Liquidation safety
- Интеграционный тест (идеальные условия, плохие условия, unsafe)
- update_config()
- Edge cases: negative Kelly edge, zero ATR, extreme drawdown (50%), EMA стабилизация,
  entry_price=0, conviction на границах, funding на пороге, taker_fee_rate=0, rationale

## 11. Научное обоснование дефолтных значений

| Параметр | Значение | Источник |
|---|---|---|
| Kelly Half | f*/2 | Thorp (2006): снижает дисперсию ~75% за ~25% expected growth |
| Drawdown tanh | C∞-sigmoid | Grossman & Zhou (1993): optimal leverage under drawdown constraints |
| Vol breakpoints | 0.1%-2.0% | Эмпирическая калибровка 1-min ATR/price (BTC/ETH/SOL) |
| Funding threshold | 0.05% per 8h | Bitget standard: elevated funding = cost ≈ 1% margin/8h at 20x |
| EMA alpha | 0.3 | Brown (1956): ~5-tick lookback для 1-2 sec tick interval |
| Taker fee | 0.06% | Bitget USDT-M futures standard taker rate |
| MMR | 0.4% | Bitget USDT-M maintenance margin rate (small positions) |

## 12. Итог

`leverage` — компактный stateful adaptive leverage engine для USDT-M futures scalping:

- Выбирает базовое плечо по режиму рынка
- Режет по ATR, drawdown, funding, adversarial risk, uncertainty
- Усиливает только через conviction
- Ограничивает сверху Half-Kelly
- Сглаживает EMA
- Считает ликвидацию (Bitget USDT-M isolated margin) и блокирует вход при недостаточном буфере

Production-grade после ревизии:
- Все формулы используют конфигурируемые параметры (нет hardcoded значений)
- Конфиг откалиброван для 1-min USDT-M scalping
- Полная валидация всех LeverageEngineConfig полей
- Удалены мёртвые поля (`leverage_stress`, `max_leverage_drawdown_scale`)
- 13 тестов включая edge cases
- 415/415 тестов проходят
