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
| `kAtrStopMultiplier` | 2.0 | Множитель ATR для стоп-лосса |
| `kDefaultThreshold` | 0.35 | Порог conviction для входа |
| HTF свечи | 200 × 1h | История для тренд-анализа (≈8 дней) |
| LTF свечи | 200 × 1m | История для прогрева индикаторов (≈3.3 часа) |

## Последовательность запуска

1. **Загрузка конфигурации** → валидация → хеширование
2. **PairScanner** → сканирование пар → выбор лучшей
3. **Bootstrap** → загрузка 200 × 1m свечей + 200 × 1h свечей
4. **WebSocket подключение** → подписки → live данные
5. **Прогрев** → 200 тиков (~3-5 мин)
6. **Market Readiness Gate** → проверка HTF + RSI + тренд
7. **Торговля** → стратегии → решения → HTF фильтр → риск → ордера
