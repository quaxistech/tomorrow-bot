# Руководство по конфигурации Tomorrow Bot

## Структура конфигурации

Конфигурация хранится в YAML-файлах в директории `configs/`.
Каждый торговый режим имеет собственный файл конфигурации.

**ВАЖНО**: Конфигурационные файлы НЕ содержат секретов (API-ключей).
Вместо этого используются ссылки на имена переменных окружения.

## Конфигурационные файлы

| Файл | Режим | Описание |
|------|-------|----------|
| `configs/paper.yaml` | Paper | Симуляция, ордера не отправляются |
| `configs/shadow.yaml` | Shadow | Теневые расчёты, сравнение с рынком |
| `configs/testnet.yaml` | Testnet | Sandbox API Bitget |
| `configs/production.yaml` | Production | **Реальная торговля** |

## Секции конфигурации

### trading
```yaml
trading:
  mode: paper               # paper | shadow | testnet | production
  initial_capital: 10.0     # Начальный капитал (USDT)
```

### exchange
```yaml
exchange:
  endpoint_rest: "https://api.bitget.com"
  endpoint_ws: "wss://ws.bitget.com/v2/ws/public"
  api_key_ref: "BITGET_API_KEY"        # Имя переменной окружения
  api_secret_ref: "BITGET_API_SECRET"  # Имя переменной окружения
  passphrase_ref: "BITGET_PASSPHRASE"  # Имя переменной окружения
  timeout_ms: 3000                     # Таймаут REST (100-30000 мс)
```

### logging
```yaml
logging:
  level: "info"           # trace | debug | info | warn | error | critical
  structured_json: true   # false для читаемого текста
  output_path: "logs/production.log"  # Путь к файлу лога
```

Логи пишутся в директорию `logs/` (создаётся автоматически при первом запуске).
Также всегда дублируются в `stdout`.

### metrics
```yaml
metrics:
  enabled: true
  port: 9090          # 1024-65535
  path: "/metrics"    # Должен начинаться с "/"
```

### health
```yaml
health:
  enabled: true
  port: 8080
```

### risk
```yaml
risk:
  max_position_notional: 10.0   # Макс. размер позиции (USDT)
  max_daily_loss_pct: 50.0      # Макс. дневной убыток (% от капитала)
  max_drawdown_pct: 50.0        # Макс. просадка (% от капитала)
  kill_switch_enabled: true     # ОБЯЗАТЕЛЕН в production!
```

### pair_selection (новое)
```yaml
pair_selection:
  mode: "auto"                # auto | manual | hybrid
  top_n: 5                    # Кол-во лучших пар для анализа
  min_volume_usd: 500000      # Минимальный суточный объём (USDT)
  max_spread_bps: 50          # Максимальный спред (базисные пункты)
  rotation_hours: 24          # Период ротации пар (часы)
  whitelist: ""               # Ручной список пар (для manual/hybrid)
  blacklist: ""               # Исключённые пары
```

**Режимы выбора пар:**

| Режим | Описание |
|-------|----------|
| `auto` | Полностью автоматический: сканирует все пары, ранжирует по скорингу |
| `manual` | Торгует только парами из `whitelist` |
| `hybrid` | Автоматический скоринг + пары из `whitelist` всегда включены |

**Алгоритм скоринга (режим `auto`):**
- 40% — объём 24ч (нормализованный)
- 30% — волатильность (оптимальная для краткосрочной торговли)
- 20% — ATR (абсолютный диапазон движения)
- 10% — ликвидность (глубина стакана)

### trailing_stop (новое — Chandelier Exit)
```yaml
trailing_stop:
  enabled: true
  atr_multiplier_strong: 3.0      # ATR множитель при сильном тренде (ADX > 30)
  atr_multiplier_moderate: 2.5    # ATR множитель при умеренном тренде
  atr_multiplier_choppy: 1.5      # ATR множитель в choppy-рынке
  breakeven_atr_threshold: 1.5    # Перевод в breakeven при прибыли +1.5×ATR
  partial_tp_atr_threshold: 2.0   # Partial take-profit (50%) при +2×ATR
  partial_tp_pct: 50              # Процент закрытия при partial TP
```

**Правила Chandelier Exit:**
- Стоп-лосс подтягивается по ATR, адаптируясь к силе тренда (ADX)
- При ADX > 30: широкий стоп (3×ATR) для удержания позиции в тренде
- При ADX 20-30: средний стоп (2.5×ATR)
- При ADX < 20: узкий стоп (1.5×ATR) для быстрого выхода в choppy-рынке
- Стоп **только подтягивается**, никогда не откатывается назад
- Breakeven: при достижении прибыли +1.5×ATR стоп перемещается на точку входа
- Partial TP: при +2×ATR закрывается 50% позиции

### volatility_targeting (новое)
```yaml
volatility_targeting:
  enabled: true
  target_annual_vol: 0.15         # Целевая годовая волатильность (15%)
  kelly_fraction: 0.25            # Доля Kelly criterion (консервативная)
  min_regime_mult: 0.1            # Минимальный множитель режима
  max_regime_mult: 1.0            # Максимальный множитель режима
```

**Формула:** `size = target_vol / realized_vol × kelly_fraction × regime_mult`

13 режимов рынка имеют свои множители от 0.1 (опасные режимы) до 1.0 (благоприятные).

### alpha_decay (новое — расширенное)
```yaml
alpha_decay:
  enabled: true
  check_interval_sec: 60          # Интервал проверки деградации (секунды)
  reduce_weight_mult: 0.7         # Множитель веса при ReduceWeight
  reduce_size_mult: 0.5           # Множитель размера при ReduceSize
  reduce_size_threshold_adj: 0.05 # Добавка к порогу при ReduceSize
  raise_threshold_adj: 0.10       # Добавка к порогу при RaiseThresholds
  disable_mult: 0.0               # Множитель при Disable (полное отключение)
```

### cusum (новое)
```yaml
cusum:
  drift: 0.5                      # Drift параметр (в единицах σ)
  threshold: 4.0                  # Порог обнаружения (в единицах σ)
  min_samples: 30                 # Минимум наблюдений для расчёта
```

### vpin (новое)
```yaml
vpin:
  toxicity_threshold: 0.7         # Порог токсичности (0.0-1.0)
  bucket_size: 100                # Размер volume bucket
  calibration_trades: 10          # Кол-во трейдов для начальной калибрации
```

### volume_profile (новое)
```yaml
volume_profile:
  price_levels: 50                # Кол-во ценовых уровней
  lookback_trades: 5000           # Максимум трейдов для анализа
  recalc_interval: 100            # Интервал пересчёта (трейды)
  value_area_pct: 70              # Процент объёма для Value Area
```

### twap (новое)
```yaml
twap:
  enabled: true
  min_slices: 3                   # Минимум слайсов
  max_slices: 10                  # Максимум слайсов
  front_loading_mult: 1.2         # Множитель первого слайса
  adaptive_interval: true         # Адаптивный интервал (спред + VPIN)
```

### time_of_day (новое)
```yaml
time_of_day:
  enabled: true
  quiet_hours_threshold_adj: 0.05 # Добавка к порогу в тихие часы
  sessions:                       # UTC-часы сессий
    asian: [0, 8]
    european: [8, 16]
    american: [16, 24]
```

### ml (новое — ML/AI модули)
```yaml
ml:
  entropy_filter:
    enabled: true
    threshold: 0.85               # Порог Shannon entropy (0.0-1.0)
    channels: ["returns", "volume", "spread", "flow"]
    cache_ttl_ms: 1000            # Время жизни кеша (мс)

  fingerprint:
    enabled: true
    buckets_per_dim: 5            # Buckets на измерение (5^5 = 3125 паттернов)
    edge_threshold: -0.1          # Минимальный edge для разрешения входа
    dimensions: ["spread", "imbalance", "flow", "volatility", "depth"]

  liquidation_cascade:
    enabled: true
    threshold: 0.6                # Порог composite score
    velocity_weight: 0.4          # Вес velocity
    volume_weight: 0.3            # Вес volume spike
    depth_weight: 0.3             # Вес depth thinning

  correlation:
    enabled: true
    short_window: 20              # Короткое окно (тиков)
    long_window: 100              # Длинное окно (тиков)
    decorrelation_mult: 0.5       # risk_mult при decorrelation
    reference_symbols: ["BTC", "ETH"]

  bayesian:
    enabled: true
    regime_weight: 0.7            # Вес текущего режима (0.0-1.0)
    exploration_rate: 0.1         # Доля Thompson Sampling exploration
    prior_mean: 0.0               # Prior mean (Normal-Normal conjugate)
    prior_variance: 1.0           # Prior variance

  thompson_sampler:
    enabled: true
    arms: 5                       # Кол-во arms (EnterNow, Wait1/2/3, Skip)
    decay: 0.995                  # Exponential decay
    initial_alpha: 1.0            # Начальный α (Beta distribution)
    initial_beta: 1.0             # Начальный β (Beta distribution)
```

## Настройка API-ключей

Создайте файл `.env` в корне проекта:

```bash
BITGET_API_KEY=bg_xxxxxxxxxxxxxxxxxxxxxxxxxxxxx
BITGET_API_SECRET=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
BITGET_PASSPHRASE=ваш_passphrase
```

Бот автоматически загружает `.env` при запуске. Файл защищён `.gitignore`.

## Режимы торговли

| Режим | Исполнение | Рекомендуемые риск-лимиты |
|-------|-----------|--------------------------|
| `paper` | `PaperOrderSubmitter` (симуляция) | Свободные |
| `shadow` | `PaperOrderSubmitter` + теневое сравнение | Как в production |
| `testnet` | `BitgetOrderSubmitter` (sandbox API) | Умеренные |
| `production` | `BitgetOrderSubmitter` (реальные деньги) | **Строгие** |

## Хеш конфигурации

При каждом запуске вычисляется SHA-256 хеш файла конфигурации.
Хеш записывается в лог и метрики для аудита изменений.

## Параметры торгового конвейера (hardcoded)

Следующие параметры определены в коде и не конфигурируются через YAML:

| Параметр | Значение | Описание |
|----------|----------|----------|
| `kMinWarmupTicks` | 200 | Минимум тиков перед началом торговли (~3-5 мин) |
| `kOrderCooldownNs` | 30 сек | Интервал между ордерами |
| `kStopLossCooldownNs` | 5 сек | Интервал между стоп-лоссами |
| `kMaxLossPerTradePct` | 1.0% | Максимальный убыток на сделку |
| `kAtrStopMultiplier` | 2.0 (base) | Базовый множитель ATR для стоп-лосса |
| `kAtrStopMultiplierStrong` | 3.0 | ATR множитель при сильном тренде (ADX > 30) |
| `kAtrStopMultiplierModerate` | 2.5 | ATR множитель при умеренном тренде |
| `kAtrStopMultiplierChoppy` | 1.5 | ATR множитель в choppy-рынке |
| `kBreakevenAtrThreshold` | 1.5 | ATR прибыли для перевода в breakeven |
| `kPartialTpAtrThreshold` | 2.0 | ATR прибыли для частичного закрытия (50%) |
| `kDefaultThreshold` | 0.35 | Базовый порог conviction для входа |
| `kAlphaDecayCheckIntervalSec` | 60 | Интервал проверки alpha decay (секунды) |
| `kTargetAnnualVol` | 0.15 | Целевая годовая волатильность (Volatility Targeting) |
| `kKellyFraction` | 0.25 | Доля Kelly criterion (консервативная) |
| `kEntropyThreshold` | 0.85 | Порог Shannon entropy для блокировки торговли |
| `kCusumDrift` | 0.5σ | CUSUM drift параметр |
| `kCusumThreshold` | 4.0σ | CUSUM порог обнаружения |
| `kVpinToxicityThreshold` | 0.7 | VPIN порог токсичности |
| `kVpinCalibrationTrades` | 10 | Кол-во трейдов для калибрации VPIN |
| `kVolumeProfileLevels` | 50 | Кол-во ценовых уровней Volume Profile |
| `kVolumeProfileLookback` | 5000 | Макс. трейдов для Volume Profile |
| `kVolumeProfileRecalcInterval` | 100 | Интервал пересчёта Volume Profile (трейды) |
| `kTwapMinSlices` | 3 | Минимум слайсов TWAP |
| `kTwapMaxSlices` | 10 | Максимум слайсов TWAP |
| `kTwapFrontLoadingMult` | 1.2 | Множитель front-loading первого слайса |
| `kTodQuietHoursAdj` | 0.05 | Добавка к порогу conviction в тихие часы |
| `kFingerprintBuckets` | 5 | Кол-во buckets на измерение (5^5 = 3125 паттернов) |
| `kFingerprintEdgeThreshold` | -0.1 | Минимальный edge для входа |
| `kCascadeThreshold` | 0.6 | Порог каскадной ликвидации |
| `kCascadeVelocityWeight` | 0.4 | Вес velocity в cascade score |
| `kCascadeVolumeWeight` | 0.3 | Вес volume spike в cascade score |
| `kCascadeDepthWeight` | 0.3 | Вес depth thinning в cascade score |
| `kCorrelationShortWindow` | 20 | Короткое окно корреляции (тиков) |
| `kCorrelationLongWindow` | 100 | Длинное окно корреляции (тиков) |
| `kCorrelationDecorrelationMult` | 0.5 | risk_mult при decorrelation |
| `kBayesianRegimeWeight` | 0.7 | Вес текущего режима в Bayesian adaptation |
| `kBayesianExplorationRate` | 0.1 | Доля Thompson Sampling exploration |
| `kThompsonArms` | 5 | Кол-во arms (EnterNow, Wait1/2/3, Skip) |
| `kThompsonDecay` | 0.995 | Exponential decay для non-stationarity |
| HTF свечи | 200 × 1h | История для тренд-анализа (≈8 дней) |
| HTF обновление | 60 мин | Периодический пересчёт HTF + экстренный при > 3×ATR |
| LTF свечи | 200 × 1m | История для прогрева индикаторов (≈3.3 часа) |

## Последовательность запуска

1. **Загрузка конфигурации** → валидация → хеширование
2. **PairScanner** → сканирование пар → выбор лучшей
3. **Bootstrap** → загрузка 200 × 1m свечей + 200 × 1h свечей
4. **WebSocket подключение** → подписки → live данные
5. **Прогрев** → 200 тиков (~3-5 мин)
6. **Инициализация ML** → Bayesian priors, Thompson arms, Entropy baseline
7. **Market Readiness Gate** → проверка HTF + RSI + тренд
8. **Торговля** → стратегии → ML фильтры → решения → HTF фильтр → volatility targeting → риск → trailing stop → TWAP → ордера
9. **HTF Real-Time Update** → пересчёт каждый час (фон)
10. **Alpha Decay Feedback** → проверка каждые 60с (фон)
