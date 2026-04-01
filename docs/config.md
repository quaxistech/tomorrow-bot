# Конфигурация

Конфигурация загружается из YAML-файла, указанного через `--config` (по умолчанию `configs/paper.yaml`).
Секреты хранятся в `.env`, в конфиге — только ссылки на имена переменных.

---

## Файлы конфигурации

| Файл | Назначение |
|------|------------|
| `configs/paper.yaml` | Бумажная торговля (симуляция) |
| `configs/testnet.yaml` | Тестовая сеть Bitget |
| `configs/shadow.yaml` | Теневой режим |
| `configs/production.yaml` | Реальная торговля |

---

## Секции конфигурации

### `trading`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `mode` | string | Режим: `paper`, `testnet`, `shadow`, `production` |
| `initial_capital` | double | Начальный капитал (USDT) |

### `exchange`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `endpoint_rest` | string | URL REST API (`https://api.bitget.com`) |
| `endpoint_ws` | string | URL WebSocket (`wss://ws.bitget.com/v2/ws/public`) |
| `api_key_ref` | string | Имя env-переменной для API key |
| `api_secret_ref` | string | Имя env-переменной для API secret |
| `passphrase_ref` | string | Имя env-переменной для passphrase |
| `timeout_ms` | int | Таймаут REST-запросов (мс) |

### `logging`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `level` | string | Уровень: `debug`, `info`, `warn`, `error` |
| `structured_json` | bool | JSON-формат логов |
| `output_path` | string | Путь к файлу логов |

### `metrics`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `enabled` | bool | Включить Prometheus-экспортёр |
| `port` | int | Порт метрик (default: 9090) |
| `path` | string | URL path (`/metrics`) |

### `risk`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `max_position_notional` | double | Макс. нотионал позиции (USDT) |
| `max_daily_loss_pct` | double | Макс. дневной убыток (%) |
| `max_drawdown_pct` | double | Макс. просадка (%) |
| `kill_switch_enabled` | bool | Аварийный выключатель |
| `max_strategy_daily_loss_pct` | double | Макс. дневной убыток стратегии (%) |
| `max_strategy_exposure_pct` | double | Макс. экспозиция стратегии (%) |
| `max_symbol_concentration_pct` | double | Макс. концентрация на символ (%) |
| `max_same_direction_positions` | int | Макс. позиций в одном направлении |
| `regime_aware_limits_enabled` | bool | Адаптация лимитов по режиму |
| `stress_regime_scale` | double | Множитель при стрессе |
| `trending_regime_scale` | double | Множитель в тренде |
| `chop_regime_scale` | double | Множитель в боковике |
| `max_trades_per_hour` | int | Макс. сделок в час |
| `min_trade_interval_sec` | double | Мин. интервал между сделками (с) |
| `max_adverse_excursion_pct` | double | Макс. неблагоприятное отклонение (%) |
| `max_realized_daily_loss_pct` | double | Макс. реализованный дневной убыток (%) |
| `utc_cutoff_hour` | int | Час UTC для остановки (-1 = off) |

### `pair_selection`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `mode` | string | `auto` (скоринг) или `manual` (ручной список) |
| `top_n` | int | Количество выбираемых пар |
| `min_volume_usdt` | double | Мин. 24ч объём (USDT) |
| `max_spread_bps` | double | Макс. спред (базисные пункты) |
| `rotation_interval_hours` | int | Интервал ротации (часы) |
| `blacklist` | string | Запрещённые пары (через запятую) |

### `decision`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `min_conviction_threshold` | double | Мин. порог убеждённости для входа |
| `conflict_dominance_threshold` | double | Порог доминирования при конфликтах |

### `trading_params`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `atr_stop_multiplier` | double | Множитель ATR для стоп-лосса |
| `max_loss_per_trade_pct` | double | Макс. убыток на сделку (%) |
| `breakeven_atr_threshold` | double | ATR-порог для breakeven |
| `partial_tp_atr_threshold` | double | ATR-порог для частичного TP |
| `partial_tp_fraction` | double | Доля закрытия при частичном TP |
| `max_hold_loss_minutes` | int | Макс. время удержания убыточной позиции |
| `max_hold_absolute_minutes` | int | Макс. абсолютное время удержания |
| `order_cooldown_seconds` | int | Пауза между ордерами |

### `execution_alpha`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `max_spread_bps_passive` | double | Макс. спред для passive execution |
| `max_spread_bps_any` | double | Макс. спред для любого execution |
| `adverse_selection_threshold` | double | Порог adverse selection |
| `urgency_passive_threshold` | double | Порог urgency для passive |
| `urgency_aggressive_threshold` | double | Порог urgency для aggressive |
| `vpin_toxic_threshold` | double | VPIN порог токсичности |
| `use_weighted_mid_price` | bool | Взвешенная mid price |
| `postonly_spread_threshold_bps` | double | Порог спреда для post-only |

### `opportunity_cost`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `min_net_expected_bps` | double | Мин. ожидаемая доходность (bps) |
| `execute_min_net_bps` | double | Мин. net edge для исполнения |
| `max_symbol_concentration` | double | Макс. концентрация на символ |
| `max_strategy_concentration` | double | Макс. концентрация на стратегию |
| `capital_exhaustion_threshold` | double | Порог исчерпания капитала |

### `regime`

Параметры классификатора рыночных режимов:

| Группа | Ключевые параметры |
|--------|-------------------|
| Тренд | `trend_adx_strong`, `trend_adx_weak_min/max`, `trend_rsi_bias` |
| Mean reversion | `mr_rsi_overbought/oversold`, `mr_adx_max` |
| Волатильность | `vol_bb_expansion/compression`, `vol_atr_expansion` |
| Стресс | `stress_rsi_extreme_high/low`, `stress_spread_bps`, `stress_book_instability` |
| Chop | `chop_adx_max` |
| Гистерезис | `transition_confirmation_ticks`, `transition_min_confidence`, `transition_dwell_ticks` |
| Уверенность | `confidence_base`, `stability_same_regime` |

### `world_model`

9 сценариев мира с индивидуальными порогами:

| Сценарий | Ключевые параметры |
|----------|-------------------|
| `toxic_microstructure` | `book_instability_min`, `aggressive_flow_min`, `spread_bps_min` |
| `liquidity_vacuum` | `spread_bps_critical/secondary`, `liquidity_ratio_min` |
| `exhaustion_spike` | `rsi_upper/lower`, `momentum_abs_min` |
| `fragile_breakout` | `bb_percent_b_upper/lower`, `volatility_5_min` |
| `compression` | `bb_bandwidth_max`, `atr_normalized_max` |
| `stable_trend` | `adx_min`, `rsi_lower/upper` |
| `chop_noise` | `adx_max`, `rsi_lower/upper` |

Плюс: `fragility` (веса), `hysteresis`, `history`, `persistence`, `suitability`, `feedback`.

### `adversarial_defense`

40+ параметров защиты от рыночных аномалий:

| Группа | Описание |
|--------|----------|
| Базовые | `spread_explosion_threshold_bps`, `min_liquidity_depth`, `book_imbalance_threshold` |
| VPIN | `vpin_toxic_threshold`, `toxic_flow_ratio_threshold` |
| Adaptive baseline | `baseline_alpha`, `z_score_*_threshold` |
| Threat memory | `threat_memory_alpha`, `threat_escalation_*` |
| Percentile | `percentile_window_size`, `percentile_severity_threshold` |
| Correlation | `correlation_alpha`, `correlation_breakdown_threshold` |
| Multi-timeframe | `baseline_halflife_fast/medium/slow_ms` |
| Hysteresis | `hysteresis_enter/exit_severity` |

### `futures`

| Параметр | Тип | Описание |
|----------|-----|----------|
| `enabled` | bool | Фьючерсный режим |
| `product_type` | string | `USDT-FUTURES` |
| `margin_mode` | string | `isolated` |
| `default_leverage` | int | Плечо по умолчанию |
| `max_leverage` | int | Макс. плечо |
| `leverage_trending` | int | Плечо в тренде |
| `leverage_ranging` | int | Плечо в боковике |
| `leverage_volatile` | int | Плечо при волатильности |
| `leverage_stress` | int | Плечо при стрессе |
| `liquidation_buffer_pct` | double | Буфер до ликвидации (%) |
| `funding_rate_threshold` | double | Порог funding rate |
| `funding_rate_penalty` | double | Штраф за высокий funding |
| `maintenance_margin_rate` | double | Ставка поддерживающей маржи |

---

## Переменные окружения

| Переменная | Описание |
|------------|----------|
| `BITGET_API_KEY` | API key Bitget |
| `BITGET_API_SECRET` | API secret Bitget |
| `BITGET_PASSPHRASE` | Passphrase Bitget |
| `POSTGRES_URL` | Строка подключения PostgreSQL |
| `TOMORROW_BOT_PRODUCTION_CONFIRM` | Подтверждение production-режима |

Секреты загружаются из `.env` файла в корне проекта через `EnvSecretProvider`.

---

## Разница между режимами

| Параметр | Paper | Production |
|----------|-------|------------|
| `initial_capital` | 10.0 | 4.58 |
| `pair_selection.top_n` | 5 | 1 |
| `futures.default_leverage` | 3 | 20 |
| `futures.max_leverage` | 10 | 20 |
| `risk.max_daily_loss_pct` | 2.0 | 10.0 |
| `futures.liquidation_buffer_pct` | 8.0 | 3.0 |
