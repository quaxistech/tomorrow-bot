# Руководство по симуляции сценариев

## Обзор

Модуль `SyntheticScenarioSimulator` генерирует стресс-сценарии для проверки
устойчивости системы к враждебным рыночным условиям.

---

## 1. Предустановленные сценарии (9 шт.)

| # | Категория           | Описание                                       |
|---|---------------------|-------------------------------------------------|
| 1 | SpreadExplosion     | Резкий рост спреда от нормального до критического |
| 2 | StaleFeedBurst      | Серия устаревших рыночных данных                 |
| 3 | ReconnectStorm      | Множественные потери и восстановления соединения  |
| 4 | OrderBookDesync     | Рассинхронизация стакана с биржей                |
| 5 | ExchangeSlowdown    | Постепенное замедление ответов биржи             |
| 6 | RejectStorm         | Массовое отклонение ордеров биржей               |
| 7 | LiquidityCollapse   | Обвал ликвидности — глубина стакана → 0          |
| 8 | AITimeoutStorm      | Серия таймаутов AI-сервиса                       |
| 9 | ExecutionToxicity   | Токсичное исполнение — систематический slippage   |

---

## 2. Генерация шагов

Каждый сценарий генерирует последовательность `ScenarioStep`:

```cpp
SyntheticScenarioSimulator simulator;

ScenarioConfig config;
config.category = ScenarioCategory::SpreadExplosion;
config.duration_steps = 10;
config.intensity = 0.8;  // [0=мягкий, 1=экстремальный]

auto steps = simulator.generate_scenario(config);
```

### Параметры генерации

- `duration_steps` — количество шагов сценария (по умолчанию 10)
- `intensity` — интенсивность [0.0, 1.0]
- `parameters` — специфичные для категории параметры

### Структура шага

```cpp
struct ScenarioStep {
    int step_number;
    MarketCondition market_condition;  // Рыночная обстановка
    bool feed_stale;                   // Данные устарели
    bool connection_lost;              // Соединение потеряно
    bool exchange_slow;                // Биржа замедлена
    bool order_rejected;               // Ордер отклонён
    int64_t simulated_latency_ms;      // Задержка (мс)
};
```

---

## 3. Ожидаемые реакции

Для каждого шага определяется ожидаемая реакция системы:

```cpp
auto reaction = simulator.get_expected_reaction(category, step);

// reaction.should_veto_trade       — запретить торговлю
// reaction.should_enter_degraded   — войти в деградацию
// reaction.should_trigger_cooldown — активировать cooldown
// reaction.should_alert_operator   — уведомить оператора
// reaction.should_activate_kill_switch — kill switch
```

### Примеры ожиданий

| Сценарий          | Высокий спред   | Низкая ликвидность | Невалидный стакан |
|-------------------|-----------------|--------------------|--------------------|
| SpreadExplosion   | veto + alert    | —                  | —                  |
| LiquidityCollapse | —               | veto               | —                  |
| OrderBookDesync   | —               | —                  | veto               |

---

## 4. Прогон с AdversarialMarketDefense

Полный прогон сценария с оценкой реакции защитной системы:

```cpp
SyntheticScenarioSimulator simulator;
AdversarialMarketDefense defense;

auto result = simulator.run_scenario(config, defense);

// result.total_steps                  — всего шагов
// result.steps_with_correct_reaction  — правильных реакций
// result.safety_maintained            — безопасность сохранена
// result.degraded_mode_triggered      — деградация была
// result.issues                       — обнаруженные проблемы
// result.observations                 — наблюдения
```

### Критерии успеха

- `safety_maintained == true` — система не допустила опасных действий
- `steps_with_correct_reaction / total_steps > 0.8` — 80%+ правильных реакций
- `issues.empty()` — нет критических проблем

---

## 5. Запуск всех сценариев

```cpp
auto presets = SyntheticScenarioSimulator::get_preset_scenarios();
AdversarialMarketDefense defense;

for (const auto& preset : presets) {
    auto result = simulator.run_scenario(preset, defense);
    // Проверить result.safety_maintained
}
```

### Интеграция с CI

Все 9 сценариев запускаются как часть тестового набора.
Тесты проверяют что `safety_maintained == true` для каждого сценария.

---

## 6. Создание пользовательских сценариев

```cpp
ScenarioConfig custom;
custom.category = ScenarioCategory::SpreadExplosion;
custom.name = "extreme_spread";
custom.duration_steps = 20;
custom.intensity = 1.0;
custom.parameters["initial_spread_bps"] = 10.0;
custom.parameters["final_spread_bps"] = 500.0;

auto result = simulator.run_scenario(custom, defense);
```
