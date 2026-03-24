# Руководство по развёртыванию в Production

## Обзор

Пошаговое руководство по сборке, установке и запуску Tomorrow Bot
в production-окружении для реальной торговли на Bitget.

---

## 1. Требования к среде

### Операционная система

- Ubuntu 24.04 LTS (рекомендуется)
- GCC 13.3+ с поддержкой C++23

### Зависимости

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake \
    libboost-all-dev libssl-dev
```

---

## 2. Сборка

```bash
cd ~/projects/tomorrow-bot

# Debug-сборка (рекомендуется для начала)
mkdir -p build-check && cd build-check
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)

# Прогон тестов — ВСЕ 203 должны пройти
ctest --output-on-failure -j8

# Release-сборка (для длительной работы)
cd ~/projects/tomorrow-bot
mkdir -p build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
ctest --output-on-failure -j8
```

### Профили сборки

| Профиль   | Описание                       | Флаг CMake                  |
|-----------|--------------------------------|-----------------------------|
| Debug     | Отладка, все проверки          | `-DCMAKE_BUILD_TYPE=Debug`  |
| Release   | Оптимизированная production    | `-DCMAKE_BUILD_TYPE=Release`|
| ASan      | Address Sanitizer              | `-DSANITIZER=address`       |
| TSan      | Thread Sanitizer               | `-DSANITIZER=thread`        |

---

## 3. Настройка API-ключей

Создайте файл `.env` в корне проекта:

```bash
cd ~/projects/tomorrow-bot
cat > .env << 'EOF'
BITGET_API_KEY=ваш_api_key
BITGET_API_SECRET=ваш_secret_key
BITGET_PASSPHRASE=ваш_passphrase
EOF
chmod 600 .env
```

Файл `.env` защищён `.gitignore` и не попадёт в репозиторий.

---

## 4. Конфигурация

Отредактируйте `configs/production.yaml` под ваш аккаунт:

```yaml
trading:
  mode: production
  initial_capital: 10.0        # Сумма на счёте (USDT)

risk:
  max_position_notional: 10.0  # Макс. размер позиции
  max_daily_loss_pct: 50.0     # Макс. дневной убыток (%)
  max_drawdown_pct: 50.0       # Макс. просадка (%)
  kill_switch_enabled: true     # ОБЯЗАТЕЛЬНО для production
```

---

## 5. Предпродакшен чеклист

- [ ] Все 203 теста проходят (`ctest` без ошибок)
- [ ] API-ключи Bitget установлены в `.env`
- [ ] Конфигурация `configs/production.yaml` проверена
- [ ] Лимиты риска настроены под ваш капитал
- [ ] `kill_switch_enabled: true`
- [ ] Paper-режим протестирован (30+ секунд без ошибок)
- [ ] Директория `logs/` существует
- [ ] WebSocket-соединение стабильно (1000+ msg/min)

---

## 6. Запуск

### Быстрый запуск (из терминала)

```bash
cd ~/projects/tomorrow-bot

# Бумажная торговля (проверка)
./build-check/src/app/tomorrow-bot -c configs/paper.yaml

# Реальная торговля
./build-check/src/app/tomorrow-bot -c configs/production.yaml
```

### Фоновый запуск (nohup)

```bash
cd ~/projects/tomorrow-bot
nohup ./build-check/src/app/tomorrow-bot -c configs/production.yaml \
    >> logs/production.log 2>&1 &
echo $! > logs/tomorrow-bot.pid
```

### Остановка

```bash
# Graceful shutdown через SIGTERM
kill $(cat logs/tomorrow-bot.pid)

# Или по Ctrl+C в терминале
```

### Systemd (серверное развёртывание)

```bash
sudo cp deploy/systemd/tomorrow-bot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable tomorrow-bot
sudo systemctl start tomorrow-bot

# Проверка
sudo systemctl status tomorrow-bot
sudo journalctl -u tomorrow-bot -f
```

---

## 7. Мониторинг

### Логи

```bash
# Живой вывод
tail -f logs/production.log

# Поиск ордеров
grep "ОРДЕР ОТПРАВЛЕН" logs/production.log

# Поиск ошибок
grep '"level":"ERROR"' logs/production.log
```

### Ключевые сообщения в логах

| Сообщение | Значение |
|-----------|----------|
| `Прогрев завершён` | Feature Engine готов (51 свеча собрана) |
| `Статус рынка` | Периодический отчёт (каждые ~30 секунд) |
| `ОРДЕР ОТПРАВЛЕН` | Ордер отправлен на биржу |
| `Сделка отклонена риск-движком` | Risk Engine заблокировал ордер |
| `Tomorrow Bot завершил работу` | Graceful shutdown завершён |

### Prometheus + Grafana

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'tomorrow-bot'
    static_configs:
      - targets: ['localhost:9090']
    scrape_interval: 5s
```

---

## 8. Аварийные процедуры

### Kill Switch

Kill switch автоматически активируется при превышении лимитов.
Для ручной остановки:

```bash
kill -SIGTERM $(cat logs/tomorrow-bot.pid)
```

### Проблемы с WebSocket

Бот автоматически переподключается. Если данные не поступают 5+ секунд,
Risk Engine блокирует ордера (stale feed guard).

### Проблемы с API-ключами

```bash
# Проверьте .env
cat .env

# Проверьте что ключи валидны через curl
curl -s https://api.bitget.com/api/v2/spot/account/assets \
  -H "ACCESS-KEY: $BITGET_API_KEY" | head -100
```
