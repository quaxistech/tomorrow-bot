# Конечный автомат состояний ордера (Order FSM)

## Обзор

Каждый ордер управляется конечным автоматом (`OrderFSM`),
который обеспечивает корректность переходов между состояниями.

## Состояния

| Состояние | Описание | Терминальное | Активное |
|-----------|----------|:---:|:---:|
| `New` | Создан, не отправлен | ✗ | ✗ |
| `PendingAck` | Отправлен, ожидаем подтверждение | ✗ | ✓ |
| `Open` | Активен в стакане | ✗ | ✓ |
| `PartiallyFilled` | Частично исполнен | ✗ | ✓ |
| `Filled` | Полностью исполнен | ✓ | ✗ |
| `CancelPending` | Запрос отмены отправлен | ✗ | ✓ |
| `Cancelled` | Отменён | ✓ | ✗ |
| `Rejected` | Отклонён биржей | ✓ | ✗ |
| `Expired` | Истёк | ✓ | ✗ |
| `UnknownRecovery` | Неизвестное состояние (восстановление) | ✗ | ✗ |

## Допустимые переходы

```
New ──────────────────────────────► PendingAck
                                        │
                    ┌───────────────────┬┴──────────────┬──────────────┐
                    ▼                   ▼               ▼              ▼
                  Open              Rejected          Filled    UnknownRecovery
                    │                  (T)              (T)
        ┌──────────┬┴──────────┬──────────┐
        ▼          ▼           ▼          ▼
   PartiallyFilled Filled  CancelPending Expired
        │          (T)         │          (T)
   ┌────┴────┐            ┌───┴────┬──────┬──────┐
   ▼         ▼            ▼        ▼      ▼      ▼
 Filled  CancelPending  Cancelled Filled Partial UnknownRecovery
  (T)        │            (T)      (T)
             │
        (те же переходы)
```

(T) = Терминальное состояние (из него нельзя перейти)

## Таблица переходов

| Из ↓ / В → | PendingAck | Open | Partial | Filled | CancelPending | Cancelled | Rejected | Expired | Recovery |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| New | ✓ | | | | | | | | |
| PendingAck | | ✓ | | ✓ | | | ✓ | | ✓ |
| Open | | | ✓ | ✓ | ✓ | | | ✓ | ✓ |
| PartiallyFilled | | | | ✓ | ✓ | | | ✓ | ✓ |
| CancelPending | | | ✓ | ✓ | | ✓ | | | ✓ |
| Filled | | | | | | | | | |
| Cancelled | | | | | | | | | |
| Rejected | | | | | | | | | |
| Expired | | | | | | | | | |
| Recovery | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

## Типичные сценарии

### Счастливый путь (Limit Order)
```
New → PendingAck → Open → PartiallyFilled → Filled
```

### Немедленное заполнение (Market Order)
```
New → PendingAck → Filled
```

### Отмена
```
New → PendingAck → Open → CancelPending → Cancelled
```

### Отклонение
```
New → PendingAck → Rejected
```

### Истечение
```
New → PendingAck → Open → Expired
```

## Реализация

- **Файл**: `src/execution/order_fsm.hpp`, `src/execution/order_fsm.cpp`
- **Проверка**: `is_valid_transition(from, to)` в `src/execution/order_types.cpp`
- **История**: Каждый FSM хранит историю переходов (`OrderTransition`)
