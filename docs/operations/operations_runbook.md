# Операционный Runbook — Tomorrow Bot

## Обзор

Данный документ описывает стандартные операционные процедуры для системы Tomorrow Bot.

---

## 1. Запуск системы

### Предпусковой чеклист

- [ ] Конфигурация загружена (`configs/` + `.env`)
- [ ] API-ключи установлены и проверены
- [ ] Health endpoint доступен на порту метрик
- [ ] Логирование настроено (уровень, формат)
- [ ] Kill switch деактивирован
- [ ] Safe mode выключен (если не первый запуск)

### Команда запуска

```bash
# Через systemd
sudo systemctl start tomorrow-bot

# Вручную
./build/src/app/tomorrow_bot --config configs/production.yaml
```

### Проверка после запуска

- Лог: `"Система успешно запущена"`
- Health probe: `curl http://localhost:9090/health` → `ready`
- Метрики: `curl http://localhost:9090/metrics` → Prometheus формат

---

## 2. Мониторинг

### Health Probes

| Эндпоинт            | Описание                         |
|----------------------|----------------------------------|
| `/health`            | Общее состояние (ready/degraded) |
| `/health/subsystems` | Состояние каждой подсистемы      |
| `/metrics`           | Prometheus метрики                |

### Ключевые метрики

- `tb_supervisor_state` — текущее состояние супервизора
- `tb_order_latency_ms` — задержка исполнения ордеров
- `tb_spread_bps` — текущий спред
- `tb_risk_utilization` — утилизация лимитов риска

### Логи

- Уровень `info` — нормальная работа
- Уровень `warn` — деградация, аномалии
- Уровень `error` — ошибки, требующие внимания

---

## 3. Типичные операции

### Включение/выключение стратегии

```
operator_control: disable_strategy <strategy_id> <reason>
operator_control: enable_strategy <strategy_id>
```

### Активация Kill Switch

```
operator_control: kill_switch on <reason>
```

Немедленно останавливает все торговые операции. Аудит записывается автоматически.

### Safe Mode

```
operator_control: safe_mode on
```

Режим «только чтение» — система мониторит рынок, но не торгует.

---

## 4. Деградированный режим

### Вход

Автоматически при:
- Потере соединения с биржей
- Устаревании рыночных данных
- Превышении порогов риска

### Поведение

- Новые ордера блокируются
- Существующие позиции мониторятся
- Оператор уведомляется

### Выход

Автоматически при восстановлении нормальных условий или вручную оператором.

---

## 5. Перезапуск

### Плановый перезапуск

```bash
sudo systemctl restart tomorrow-bot
```

### Аварийный перезапуск

```bash
sudo systemctl stop tomorrow-bot
# Проверить логи на предмет причины
journalctl -u tomorrow-bot --since "5 minutes ago"
# Перезапустить
sudo systemctl start tomorrow-bot
```

### После перезапуска

- Проверить health endpoint
- Проверить что все подсистемы Healthy
- Убедиться что позиции согласованы
