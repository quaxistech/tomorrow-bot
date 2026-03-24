# Поток исполнения ордеров — Фаза 4

## Обзор

Фаза 4 реализует полный путь от торгового решения до исполнения ордера.
Каждый шаг обязателен, ни один торговый путь не может обойти Риск-движок.

## Конвейер исполнения

```
DecisionRecord (из decision/)
    │
    ▼
ExecutionAlphaEngine → ExecutionAlphaResult
    │  Определяет стиль исполнения (Passive/Aggressive/Hybrid)
    │  Оценивает качество исполнения (спред, проскальзывание)
    │  Рассчитывает лимитную цену и план нарезки
    │
    ▼
OpportunityCostEngine → OpportunityCostResult
    │  Оценивает ожидаемый доход vs стоимость исполнения
    │  Решает: Execute / Defer / Suppress
    │
    ▼
PortfolioAllocator → SizingResult
    │  Применяет иерархию бюджетов
    │  Лимит концентрации (≤20% капитала на одну позицию)
    │  Лимит стратегии (≤30% капитала)
    │  Множитель неопределённости
    │  Ограничение доступным капиталом
    │
    ▼
┌─────────────────────────────────────────┐
│     RiskEngine → RiskDecision           │
│     *** ОБЯЗАТЕЛЬНЫЙ, ОБХОД ЗАПРЕЩЁН ***│
│                                         │
│  14 правил последовательной проверки:   │
│  1.  Kill switch                        │
│  2.  Макс дневной убыток              │
│  3.  Макс просадка                     │
│  4.  Макс одновременных позиций        │
│  5.  Макс валовая экспозиция           │
│  6.  Макс номинал позиции              │
│  7.  Макс плечо                        │
│  8.  Макс проскальзывание              │
│  9.  Частота ордеров                   │
│  10. Подряд убыточные сделки           │
│  11. Актуальность данных               │
│  12. Качество стакана                  │
│  13. Ширина спреда                     │
│  14. Минимальная ликвидность           │
│                                         │
│  Verdict: Approved/Denied/ReduceSize/   │
│           Throttled                     │
└─────────────────────────────────────────┘
    │
    ▼
ExecutionEngine → OrderRecord + FSM
    │  Создаёт запись ордера
    │  Управляет FSM (конечный автомат состояний)
    │  Проверяет дублирование
    │  Отправляет через IOrderSubmitter
    │
    ▼
PortfolioEngine (обновление позиций)
    │  При заполнении: открытие/обновление позиции
    │  Отслеживание P&L, экспозиции, просадки
```

## Зависимости модулей

```
tb_execution_alpha → tb_common, tb_features, tb_strategy, tb_logging, tb_clock, tb_metrics
tb_opportunity_cost → tb_common, tb_strategy, tb_execution_alpha, tb_logging, tb_clock
tb_portfolio → tb_common, tb_logging, tb_clock, tb_metrics
tb_portfolio_allocator → tb_common, tb_strategy, tb_portfolio, tb_logging
tb_risk → tb_common, tb_strategy, tb_features, tb_portfolio, tb_portfolio_allocator,
          tb_execution_alpha, tb_logging, tb_clock, tb_metrics
tb_execution → tb_common, tb_strategy, tb_risk, tb_execution_alpha, tb_portfolio,
               tb_logging, tb_clock, tb_metrics
```

## Принципы проектирования

1. **Безопасность**: Никакой живой ордер не может обойти RiskEngine
2. **Потокобезопасность**: Все движки используют мьютексы для защиты состояния
3. **Тестируемость**: Все зависимости — через интерфейсы (DI)
4. **RAII**: Используется std::lock_guard для управления мьютексами
5. **Result-based errors**: Используется std::expected вместо исключений
