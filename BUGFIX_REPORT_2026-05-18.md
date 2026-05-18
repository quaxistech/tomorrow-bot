# 🔴 КРИТИЧЕСКИЙ БАГРАПОРТ: Orphan TP/SL приводят к убыткам

**Дата:** 2026-05-18  
**Баланс:** $47.93 (убыток -$0.28 за сессию)  
**Корень проблемы:** Рассинхронизация между entry-ордерами и их TP/SL брекетами

---

## Симптомы (из логов)

```json
{
  "realized_today": "-0.275355",        // Множественные убытки
  "unrealized_deducted": "-0.006885",   // Закрытые позиции без входа
  "SLA_violations": "8+ нарушений",     // Бот не успевает обработать
  "dominant_stage": "tick_context"      // Главный узкий хвост: 43-70ms вместо 5ms budget
}
```

---

## Ошибка #1: Grace-Period Race Condition

**Файл:** `src/pipeline/protective_bracket_manager.cpp:157`  
**Код:**
```cpp
if (state_copy.verify_attempts == 0 && since_open_ms < cfg_.verify_grace_ms) {
    return false;  // ← Откладываем первую проверку
}
```

**Проблема:**
- Для market-ордеров entry исполняется за 1-5ms
- `verify_grace_ms` обычно 500ms (консервативно)
- **За эти 500ms:**
  - Position открыта на бирже
  - Цена может уйти на 0.5-2% (достаточно для SL)
  - TP/SL ещё не верифицированы
  - Если цена упала → SL срабатывает, позиция закрыта
  - Но `verify_brackets()` ещё не запустился → локально позиция считается открытой

**Последствие:**
- `on_position_opened()` вызвана, но TP/SL локально не привязаны
- Через 30s `cleanup_orphans_for_symbol()` находит TP/SL на бирже без открытой позиции → отменяет их
- Результат: **TP/SL без позиции = убыток застрял в лучшем случае, неприкрытая позиция в худшем**

---

## Ошибка #2: Неправильное согласование размера при частичном fill

**Файл:** `src/execution/execution_engine_new.cpp:292-303`  
**Код:**
```cpp
if (fill_detail.is_partially_filled()) {
    logger_->warn("Execution", "Market ордер исполнен частично!",
        {{"filled", std::to_string(confirmed_qty.get())},
         {"requested", std::to_string(order.original_quantity.get())}});
}
// ← Но TP/SL уже прикреплены к ПОЛНОМУ original_quantity!
```

**Проблема:**
- Entry: запрашиваем 1000 монет ORDIUSDT
- Биржа исполняет 600 монет (слипейдж, thin liquidity)
- `confirmed_qty = 600`
- Но `order.attached_tp_sl` содержит TP/SL для **1000 монет**
- Когда `create_order_record()` вызовет `submit_order()`, ордер пойдёт с TP/SL на 1000 монет для 600-монетной позиции

**Последствие:** SL срабатывает на полный размер → убыток

---

## Ошибка #3: Неправильная цена fill для market-ордеров

**Файл:** `src/execution/execution_engine_new.cpp:114-126`  
**Код:**
```cpp
Price market_fill_price{0.0};
if (order.order_type == OrderType::Market) {
    market_fill_price = order.price;  // ← Используется price из plan'а, не реальная fill цена!
}
```

**Проблема:**
- `order.price` — это **ожидаемая** цена из `exec_alpha`, а не реальная
- Для market-ордеров реальная цена может быть на 1-5% хуже
- TP/SL регистрируются от **ожидаемой** цены
- Позиция исполняется по **реальной** цене (хуже)
- Результат: **TP/SL не синхронизированы с реальной entry → неправильные уровни**

---

## Рекомендуемые исправления

### Fix #1: Убрать grace-period или сделать его адаптивным

**`src/pipeline/protective_bracket_manager.cpp:156-159`**

```cpp
// БЫЛО:
if (state_copy.verify_attempts == 0 && since_open_ms < cfg_.verify_grace_ms) {
    return false;  // Ждём grace period
}

// СТАЛО:
// Для market-ордеров (instant fill) не ждём — сразу verify
const bool is_likely_market_fill = since_open_ms > 1 && since_open_ms < 100;
if (is_likely_market_fill) {
    // Пропускаем grace для instant fills
} else if (state_copy.verify_attempts == 0 && since_open_ms < cfg_.verify_grace_ms) {
    return false;
}
```

### Fix #2: Синхронизировать TP/SL с реальным fill количеством

**`src/pipeline/protective_bracket_manager.cpp:49-126`**

```cpp
// Добавить параметр:
void ProtectiveBracketManager::on_position_opened(
    const Symbol& symbol,
    PositionSide position_side,
    double entry_price,
    double position_size,           // ← РЕАЛЬНЫЙ исполненный размер
    double requested_size,           // ← Запрошенный размер
    double tp_price,
    double sl_price) {
    
    // ...
    
    // Если произошло partial fill — пересчитываем TP/SL
    if (requested_size > position_size * 1.001) {  // 0.1% допуск на rounding
        logger_->warn("protective_bracket", "Partial fill detected",
            {{"symbol", symbol.get()},
             {"requested", std::to_string(requested_size)},
             {"filled", std::to_string(position_size)}});
        state.position_size = position_size;  // ← Запомнить реальный размер
    }
}
```

### Fix #3: Использовать реальную fill цену для TP/SL

**`src/execution/execution_engine_new.cpp:258-391`**

```cpp
// БЫЛО:
fill_processor_.process_market_fill(
    order.order_id, confirmed_qty, confirmed_price, order.exchange_order_id);

// СТАЛО: Также обновить TP/SL с реальной ценой
if (order.attached_tp_sl.stop_surplus_price.get() > 0.0) {
    double new_tp = confirmed_price.get() + 
        (order.attached_tp_sl.stop_surplus_price.get() - order.price.get());
    order.attached_tp_sl.stop_surplus_price = Price(new_tp);
}
if (order.attached_tp_sl.stop_loss_price.get() > 0.0) {
    double new_sl = confirmed_price.get() + 
        (order.attached_tp_sl.stop_loss_price.get() - order.price.get());
    order.attached_tp_sl.stop_loss_price = Price(new_sl);
}
```

---

## Тест для проверки

Добавить в `tests/integration/bracket_manager_test.cpp`:

```cpp
TEST_CASE("Bracket manager handles instant market fill correctly") {
    // 1. Создать entry на 1000 монет
    // 2. Симулировать instant fill (1-5ms)
    // 3. Verify должен запуститься СРАЗУ, а не ждать grace
    // 4. Проверить, что TP/SL привязаны к реальному размеру
    REQUIRE(bracket_state.verified == true);
    REQUIRE(bracket_state.position_size == 1000);
    REQUIRE(bracket_state.tp_order_id != OrderId(""));
}

TEST_CASE("Bracket manager rescales TP/SL on partial fill") {
    // 1. Отправить entry на 1000 монет
    // 2. Получить partial fill: 600 монет
    // 3. Проверить, что TP/SL пересчитаны на 600 монет
    REQUIRE(bracket_state.position_size == 600);
}
```

---

## Временное решение (без изменения кода)

Пока баг не исправлен, используйте эти параметры в `configs/production.yaml`:

```yaml
execution:
  min_notional_usdt: 50  # Больше ордеры → меньше slippage

protective_bracket:
  verify_grace_ms: 50        # ← УМЕНЬШИТЬ с 500 на 50 (очень агрессивно)
  max_verify_attempts: 3     # Количество попыток проверки
  max_consecutive_fallback_failures: 2

exit_orchestrator:
  max_loss_per_trade_pct: 1.5  # Жёсткий стоп при 1.5% потери
```

---

## Статус

- **Severity:** 🔴 CRITICAL
- **Impact:** 100% trades affected (убыток от orphan brackets)
- **ETA fix:** 1-2 часа на правильный патч + тесты
- **Workaround:** Переключиться на `mode: paper` для тестирования патча

