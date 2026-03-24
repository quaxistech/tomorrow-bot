# Руководство по добавлению новой стратегии

## Обзор

Торговые стратегии в Tomorrow Bot реализуют интерфейс `IStrategy` и генерируют
торговые намерения (`TradeIntent`). Стратегии **НЕ** размещают ордера напрямую.

## Шаги

### 1. Создайте директорию

```
src/strategy/my_strategy/
    my_strategy.hpp
    my_strategy.cpp
```

### 2. Реализуйте интерфейс IStrategy

```cpp
#pragma once
#include "strategy/strategy_interface.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <memory>

namespace tb::strategy {

class MyStrategy : public IStrategy {
public:
    MyStrategy(std::shared_ptr<logging::ILogger> logger,
               std::shared_ptr<clock::IClock> clock);

    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::atomic<bool> active_{true};
};

} // namespace tb::strategy
```

### 3. Реализуйте методы

#### `meta()` — метаданные стратегии

```cpp
StrategyMeta MyStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("my_strategy"),
        .version = StrategyVersion(1),
        .name = "MyStrategy",
        .description = "Описание стратегии",
        .preferred_regimes = {RegimeLabel::Trending},  // В каких режимах эффективна
        .required_features = {"rsi_14", "ema_20"}      // Требуемые признаки
    };
}
```

#### `evaluate()` — основная логика

```cpp
std::optional<TradeIntent> MyStrategy::evaluate(const StrategyContext& context) {
    // 1. Проверяем активность
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    // 2. Проверяем валидность данных
    const auto& tech = context.features.technical;
    if (!tech.rsi_valid) return std::nullopt;

    // 3. Генерируем сигнал
    if (/* условие входа */) {
        TradeIntent intent;
        intent.strategy_id = StrategyId("my_strategy");
        intent.strategy_version = StrategyVersion(1);
        intent.symbol = context.features.symbol;
        intent.side = Side::Buy;
        intent.conviction = 0.7;  // [0.0, 1.0]
        intent.signal_name = "my_signal";
        intent.reason_codes = {"reason1", "reason2"};
        intent.generated_at = clock_->now();
        return intent;
    }

    return std::nullopt;
}
```

### 4. Добавьте в CMakeLists.txt

В файле `src/strategy/CMakeLists.txt`:

```cmake
add_library(tb_strategy STATIC
    ...
    my_strategy/my_strategy.cpp
)
```

### 5. Зарегистрируйте стратегию

```cpp
auto registry = std::make_shared<StrategyRegistry>();
registry->register_strategy(
    std::make_shared<MyStrategy>(logger, clock));
```

### 6. Добавьте рекомендации режима

В `RuleBasedRegimeEngine::generate_hints()` добавьте записи для вашей стратегии
в каждом блоке `switch`:

```cpp
add("my_strategy", true, 1.0, "Причина рекомендации");
```

Аналогично в `RuleBasedWorldModelEngine::compute_suitability()`.

### 7. Напишите тесты

В `tests/unit/strategy/strategy_test.cpp` добавьте тесты:
- Позитивный сценарий → TradeIntent с корректными полями
- Отсутствие сигнала → nullopt
- Деактивированная стратегия → nullopt
- Граничные случаи

## Принципы

1. **Стратегия не размещает ордера** — только генерирует `TradeIntent`
2. **Детерминизм** — одинаковый `StrategyContext` → одинаковый результат
3. **Потокобезопасность** — используйте `std::atomic` для `active_`, `std::mutex` для состояния
4. **Replay** — все временны́е метки через `IClock`, не `std::chrono::system_clock`
5. **Валидация данных** — всегда проверяйте `*_valid` флаги перед использованием индикаторов
6. **Комментарии на русском** — все комментарии в коде на русском языке
