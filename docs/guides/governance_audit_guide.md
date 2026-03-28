# Руководство по Governance и аудиту

## Обзор

Модуль `GovernanceAuditLayer` обеспечивает прозрачность, контроль и аудит
всех операций системы Tomorrow Bot.

---

## 1. Типы аудиторских событий

| Тип события               | Описание                              |
|---------------------------|---------------------------------------|
| `SystemStartup`           | Запуск системы                        |
| `SystemShutdown`          | Остановка системы                     |
| `StrategyRegistered`      | Регистрация стратегии                 |
| `StrategyEnabled`         | Включение стратегии                   |
| `StrategyDisabled`        | Выключение стратегии                  |
| `StrategyVersionUpdated`  | Обновление версии стратегии           |
| `KillSwitchActivated`     | Активация аварийного выключателя      |
| `KillSwitchDeactivated`   | Деактивация аварийного выключателя    |
| `SafeModeEntered`         | Вход в безопасный режим               |
| `SafeModeExited`          | Выход из безопасного режима           |
| `ConfigChanged`           | Изменение конфигурации                |
| `TradingModeChanged`      | Изменение режима торговли             |

### Структура записи

```cpp
struct AuditEntry {
    uint64_t        audit_id;     // Уникальный идентификатор
    AuditEventType  type;         // Тип события
    std::string     actor;        // Кто инициировал
    std::string     target;       // Цель действия
    std::string     details;      // Описание
    Timestamp       timestamp;    // Время события
};
```

---

## 2. Реестр стратегий

### Регистрация

```cpp
gov.register_strategy(StrategyId("momentum_v1"), StrategyVersion(1), TradingMode::Paper);
```

### Управление

```cpp
gov.disable_strategy(StrategyId("momentum_v1"), "operator");
gov.enable_strategy(StrategyId("momentum_v1"), "operator");
gov.update_strategy_version(StrategyId("momentum_v1"), StrategyVersion(2));
```

### Информация о стратегии

```cpp
auto info = gov.get_strategy_info(StrategyId("momentum_v1"));
// info->strategy_id, info->version, info->enabled, info->mode
```

Каждое действие автоматически записывается в аудит.

---

## 3. Governance Snapshot

Снимок текущего состояния системы для диагностики:

```cpp
auto snapshot = gov.get_snapshot();
// snapshot.strategy_registry    — все зарегистрированные стратегии
// snapshot.current_config_hash  — хеш текущей конфигурации
// snapshot.runtime_version      — версия runtime
// snapshot.current_mode         — режим торговли
// snapshot.kill_switch_active   — статус kill switch
// snapshot.safe_mode_active     — статус safe mode
// snapshot.recent_audit         — последние записи аудита
```

---

## 4. Интеграция с Operator Control

### Поток команд

```
Оператор → OperatorControlPanel → GovernanceAuditLayer → Аудит
```

### Гарантии

- Каждое действие оператора записывается с идентификацией актора
- Kill switch и safe mode имеют специальные типы аудита
- Невозможно выполнить действие без записи в аудит
- Аудит неизменяем (append-only)

### Запрос аудита

```cpp
auto log = gov.get_audit_log(100);  // Последние 100 записей
```

---

## 5. Безопасность

- Аудит хранится в памяти через `MemoryStorageAdapter` (файловая/БД-персистентность — через `IStorageAdapter`)
- Каждая запись имеет уникальный монотонный ID
- Timestamps с наносекундной точностью
- Actor идентифицируется для каждого действия
