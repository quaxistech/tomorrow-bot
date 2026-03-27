# Архитектура модуля воспроизведения и бэктестирования

## Обзор

Модуль `tb::replay` обеспечивает полный цикл воспроизведения и бэктестирования:
от детерминированного проигрывания исторических событий до симуляции исполнения
с расчётом профессиональных метрик производительности.

Модуль состоит из четырёх компонентов:

| Компонент | Файлы | Назначение |
|-----------|-------|------------|
| **ReplayEngine** | `replay_engine.hpp/cpp` | Event-driven движок воспроизведения |
| **BacktestEngine** | `backtest_engine.hpp/cpp` | Оркестратор бэктеста с портфелем |
| **FillSimulator** | `fill_simulator.hpp` | Детерминированная симуляция исполнения |
| **Типы и метрики** | `replay_types.hpp`, `backtest_types.hpp` | Доменная модель |

## Сценарии использования

1. **Детерминированный анализ** — воспроизведение захваченных рыночных данных
2. **Реконструкция решений** — восстановление контекста принятия решений
3. **Post-mortem анализ** — разбор проблемных ситуаций
4. **Бэктестирование стратегий** — прогон с симуляцией исполнения и метриками
5. **Анализ challenger'ов** — сравнение альтернативных стратегий
6. **Регрессионная верификация** — проверка воспроизводимости результатов

## Состояния движка

```
Idle → Playing ⇄ Paused
         ↓
      Completed
         ↓
       Error
```

- **Idle** — начальное состояние, ожидание конфигурации
- **Playing** — воспроизведение событий (поддерживает `pause()`/`resume()`)
- **Paused** — приостановлено (можно `resume()` или `seek()`)
- **Completed** — все события воспроизведены (можно `seek()` назад)
- **Error** — ошибка при воспроизведении

## Режимы воспроизведения (ReplayMode)

Режим определяет глубину реконструкции состояния при проигрывании:

| Режим | Контексты | Назначение |
|-------|-----------|------------|
| `Inspection` | Без обогащения | Просмотр журнала |
| `Strategy` | + DecisionContext | Анализ решений стратегий |
| `Execution` | + OrderContext | Анализ жизненного цикла ордеров |
| `FullSystem` | + RiskContext, PortfolioContext | Полная реконструкция |

## Временны́е модели (ReplayTimeMode)

| Модель | Поведение |
|--------|-----------|
| `Accelerated` | Максимальная скорость — без пауз между событиями |
| `Realtime` | Воспроизведение в реальном времени |
| `Scaled` | Масштабированное время через `speed_factor` |

## Конфигурация воспроизведения

```cpp
ReplayConfig {
    start_time,              // Начало временного диапазона
    end_time,                // Конец временного диапазона
    speed_factor,            // Множитель скорости (1.0 = реальное время)
    strategy_filter,         // Опциональный фильтр по стратегии
    reconstruct_decisions,   // Реконструировать контекст решений
    emit_telemetry,          // Генерировать телеметрию при воспроизведении
    mode,                    // Режим: Inspection / Strategy / Execution / FullSystem
    time_mode,               // Модель времени: Accelerated / Realtime / Scaled
    type_filter,             // Фильтр по типу записи журнала
    max_events,              // Ограничение количества событий (0 = без лимита)
    on_event                 // Callback-hook на каждое событие
};
```

## Интерфейс ReplayEngine

```cpp
// Базовый цикл
configure(config) → start() → while(has_next()) { step() } → get_result()

// Управление воспроизведением
pause()                      // Приостановить
resume()                     // Возобновить
seek(index)                  // Перемотать к индексу события
seek_to_time(timestamp)      // Перемотать к ближайшему событию >= timestamp

// Наблюдаемость
current_index()              // Текущий индекс события (0-based)
total_events()               // Общее количество загруженных событий
progress()                   // Прогресс [0.0 .. 1.0]
get_state()                  // Текущее состояние FSM
reset()                      // Сброс в начальное состояние
```

## Rich-события (ReplayEvent)

Каждое событие обогащается контекстами в зависимости от `ReplayMode`:

```cpp
ReplayEvent {
    journal_entry,           // Исходная запись из журнала
    was_reconstructed,       // Флаг реконструкции
    sequence_index,          // Индекс в последовательности (0-based)
    simulated_time,          // Время по replay-часам

    // Контексты (заполняются в зависимости от режима)
    MarketContext  { best_bid, best_ask, mid_price, spread_bps, depth, ... }
    DecisionContext { strategy_id, signal_type, strength, confidence, thesis, ... }
    OrderContext   { order_id, symbol, side, type, price, qty, filled_qty, ... }
    PortfolioContext { total_capital, cash, exposure, unrealized_pnl, ... }
    RiskContext     { verdict, reasons, risk_utilization, kill_switch, ... }
};
```

## Результат воспроизведения (ReplayResult)

```cpp
ReplayResult {
    events_replayed,         // Количество воспроизведённых событий
    decisions_reconstructed, // Количество реконструированных решений
    simulated_duration_ns,   // Длительность симулированного периода
    wall_time_ms,            // Фактическое время выполнения

    // Разбивка по типам
    market_events, decision_events, risk_events,
    order_events, portfolio_events, system_events,

    warnings,                // Предупреждения
    final_state,             // Финальное состояние FSM
    mode                     // Использованный режим
};
```

---

## BacktestEngine

Бэктест-движок оркестрирует полный цикл бэктестирования поверх ReplayEngine:

1. Загружает исторические события через ReplayEngine
2. Обрабатывает сигналы стратегий (вход/выход)
3. Симулирует исполнение через FillSimulator
4. Ведёт портфель: cash, открытые позиции, кривая капитала
5. Вычисляет агрегированные метрики производительности

### Конфигурация бэктеста

```cpp
BacktestConfig {
    start_time, end_time,         // Период тестирования
    initial_capital,              // Начальный капитал (USD)
    fees: FeeModel { taker, maker, use_maker_for_limit },
    slippage: SlippageModel { base_bps, volume_impact, spread_mult, enabled },
    strategy_filter,              // Фильтр по стратегии
    max_concurrent_positions,     // Макс одновременных позиций
    max_position_pct              // Макс % капитала на одну позицию
};
```

### Интерфейс

```cpp
configure(config) → run() → BacktestResult
get_state()                  // Состояние внутреннего replay-движка
progress()                   // Прогресс прогона [0.0 .. 1.0]
reset()                      // Сброс состояния
```

---

## FillSimulator

Детерминированный симулятор исполнения ордеров (header-only):

- **Проскальзывание** — модель на основе спреда, объёма и глубины стакана
- **Комиссии** — раздельные maker/taker ставки с автоматическим определением
- **Частичные исполнения** — при недостаточной ликвидности
- **Limit-ордера** — проверка пересечения с лучшей ценой стакана
- **Стабильность** — без случайности, полностью воспроизводимые результаты

```cpp
FillSimulator sim(fee_model, slippage_model);
FillResult result = sim.simulate(FillRequest{
    symbol, side, order_type, price, quantity,
    best_bid, best_ask, available_depth
});
// result: filled, partial, fill_price, filled_qty, slippage_bps, fee
```

---

## Метрики производительности (PerformanceMetrics)

Функция `compute_metrics()` вычисляет профессиональный набор метрик:

| Категория | Метрики |
|-----------|---------|
| **Доходность** | Total return %, Annualized return %, Total P&L |
| **Риск** | Max drawdown %, Avg drawdown %, Max drawdown duration |
| **Risk-adjusted** | Sharpe ratio, Sortino ratio, Calmar ratio |
| **Торговые** | Win rate, Avg win/loss, Payoff ratio, Profit factor, Expectancy |
| **Серии** | Max consecutive wins, Max consecutive losses |
| **Издержки** | Total fees, Total slippage, Fees % of P&L |
| **Активность** | Avg hold duration, Turnover, Avg exposure % |

---

## Пример: полный бэктест-прогон

```cpp
auto adapter = std::make_shared<MemoryStorageAdapter>();
// ... заполнить adapter историческими данными ...

BacktestEngine engine(adapter);

BacktestConfig config;
config.start_time = Timestamp{start_ns};
config.end_time = Timestamp{end_ns};
config.initial_capital = 10000.0;
config.fees.taker_fee_pct = 0.001;
config.slippage.base_slippage_bps = 1.0;
config.max_concurrent_positions = 5;

engine.configure(config);
auto result = engine.run();

if (result && result->is_valid()) {
    const auto& m = result->metrics;
    // m.sharpe_ratio, m.max_drawdown_pct, m.win_rate, ...
}
```

## Пример: replay с callback-hooks

```cpp
ReplayEngine engine(adapter);

ReplayConfig config;
config.start_time = Timestamp{0};
config.end_time = Timestamp{end_ns};
config.mode = ReplayMode::FullSystem;
config.on_event = [](const ReplayEvent& e) {
    // Анализ каждого события в реальном времени
    if (e.risk.valid && e.risk.kill_switch_active) {
        log_warning("Kill switch активирован в момент {}", e.simulated_time.get());
    }
};

engine.configure(config);
engine.start();

while (engine.has_next()) {
    auto event = engine.step();
    // Пошаговый анализ с полным контекстом
}
```

## Архитектурные решения

- **Обратная совместимость** — старый API (`configure → start → step → get_result`) работает без изменений
- **Режимы воспроизведения** — градация Inspection < Strategy < Execution < FullSystem позволяет выбирать глубину реконструкции
- **Детерминизм** — FillSimulator не использует случайность, результаты полностью воспроизводимы
- **Потокобезопасность** — все публичные методы защищены `std::mutex`
- **Header-only FillSimulator** — минимальные зависимости, легко тестировать изолированно

## Ограничения

- Воспроизведение из in-memory хранилища (MemoryStorageAdapter); файловое — будущее расширение
- Обогащение контекстов (MarketContext, RiskContext) — на основе метаданных записей журнала; реальная реконструкция стакана и фич — будущее расширение
- BacktestEngine обрабатывает сигналы на основе DecisionContext; для полноценного бэктеста необходимы рыночные данные с ценами в payload
