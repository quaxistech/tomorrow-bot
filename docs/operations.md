# Эксплуатация

## Развёртывание

### Подготовка сервера

```bash
# Ubuntu 24.04
sudo apt update && sudo apt install -y \
  g++-14 cmake ninja-build \
  libboost-all-dev libssl-dev libpqxx-dev \
  postgresql

# Создание базы данных
sudo -u postgres psql -c "CREATE USER tbbot WITH PASSWORD 'YOUR_PASSWORD';"
sudo -u postgres psql -c "CREATE DATABASE tomorrow_bot OWNER tbbot;"
```

### Сборка

```bash
./scripts/build_release.sh
```

### Настройка окружения

Создайте `.env` в корне проекта:

```env
BITGET_API_KEY=ваш_ключ
BITGET_API_SECRET=ваш_секрет
BITGET_PASSPHRASE=ваша_парольная_фраза
POSTGRES_URL=host=localhost dbname=tomorrow_bot user=tbbot password=YOUR_PASSWORD
TOMORROW_BOT_PRODUCTION_CONFIRM=yes
```

### Systemd

Файл сервиса находится в `deploy/systemd/`. Установка:

```bash
sudo cp deploy/systemd/tomorrow-bot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable tomorrow-bot
sudo systemctl start tomorrow-bot
```

Логи:

```bash
sudo journalctl -u tomorrow-bot -f
```

---

## Запуск

### Режимы

```bash
# Paper (симуляция)
./scripts/run_paper.sh

# Production (реальные деньги!)
./build/src/app/tomorrow-bot --config configs/production.yaml
```

Production-режим требует:
- `TOMORROW_BOT_PRODUCTION_CONFIRM` в окружении
- `kill_switch_enabled: true` в конфиге

### Graceful shutdown

- `Ctrl+C` или `kill -SIGTERM <pid>`
- Таймаут: 30 секунд
- При превышении — force stop

---

## Мониторинг

### Метрики (Prometheus)

Эндпоинт: `http://localhost:9090/metrics`

Ключевые метрики:

| Метрика | Описание |
|---------|----------|
| `pipeline_tick_count` | Количество обработанных тиков |
| `pipeline_latency_ms` | Задержка обработки тика |
| `risk_checks_passed` | Пройдённые проверки риска |
| `risk_checks_failed` | Отклонённые проверки |
| `orders_submitted` | Отправленные ордера |
| `orders_filled` | Исполненные ордера |
| `portfolio_pnl_usdt` | PnL портфеля |
| `uncertainty_aggregate` | Агрегированная неопределённость |
| `scanner_duration_ms` | Время сканирования пар |

### Логи

Структурированный JSON, файл: `logs/<mode>.log`

```json
{"level":"INFO","ts":"2025-01-15T10:30:00Z","msg":"Order filled","symbol":"BTCUSDT","side":"BUY","price":97500.0,"qty":0.001}
```

---

## PostgreSQL

Таблицы создаются автоматически при первом запуске:

| Таблица | Назначение |
|---------|------------|
| `tb_journal` | Append-only журнал событий |
| `tb_snapshots` | Снапшоты состояния |

Backup:

```bash
pg_dump tomorrow_bot > backup_$(date +%Y%m%d).sql
```

---

## Ротация пар

ScannerEngine выполняет автоматическую ротацию торговых пар:

1. При старте: initial scan → top-N пар
2. Фоновый поток: rescan каждые N часов (`rotation_interval_hours`)
3. Idle monitor: если все pipeline простаивают > 30 мин — принудительный rescan
4. Orphan protection: символы с открытыми позициями добавляются автоматически

---

## Reconciliation (сверка при рестарте)

При каждом рестарте выполняется трёхступенчатая сверка с биржей:

1. **Ордера**: загрузка активных ордеров → сопоставление с внутренним состоянием
2. **Позиции**: сверка открытых позиций → восстановление или закрытие сирот
3. **Баланс**: проверка USDT-баланса

---

## Recovery (восстановление)

При crash/restart:

1. **Snapshot restore** — загрузка последнего снимка из PostgreSQL
2. **WAL replay** — воспроизведение журнала после снимка
3. **Exchange sync** — синхронизация с текущим состоянием биржи

---

## Resilience

| Компонент | Описание |
|-----------|----------|
| Circuit Breaker | Closed → Open (после N отказов) → HalfOpen → Closed |
| Retry Executor | Exponential backoff с jitter, классификация ошибок |
| Idempotency Manager | Стабильный ClientOrderId, дедупликация в 5-мин окне |

---

## Аварийные процедуры

### Kill Switch

Автоматическая активация при:
- Превышении hard drawdown
- Критической ошибке exchange
- Каскадных отказах

Ручная активация: не поддерживается в текущей версии (только программно).

При активации:
1. Broadcast на все pipeline
2. Все новые ордера блокируются
3. Существующие ордера отменяются (при наличии emergency flatten)

### Деградация биржи

- `StaleFeedCheck` блокирует торговлю при устаревших данных
- `BookQualityCheck` блокирует при невалидном ордербуке
- Circuit breaker отключает REST-запросы после серии отказов

### Потеря соединения

- WebSocket автоматически переподключается
- REST-запросы ретраятся через RetryExecutor
- При длительном disconnect — pipeline переходит в idle
