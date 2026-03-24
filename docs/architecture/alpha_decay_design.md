# Дизайн мониторинга угасания альфы (AlphaDecayMonitor)

## Обзор

AlphaDecayMonitor отслеживает деградацию производительности стратегий, обнаруживает
признаки угасания альфы и генерирует рекомендации по корректирующим действиям.

## Измерения деградации (7 измерений)

| Измерение | Описание | Метод расчёта |
|-----------|----------|---------------|
| Expectancy | Ожидаемая доходность | Среднее P&L короткого окна vs длинного |
| HitRate | Доля прибыльных сделок | % прибыльных в коротком vs длинном окне |
| SlippageAdjusted | С учётом проскальзывания | Дрифт среднего slippage |
| RegimeConditioned | С учётом режима | Производительность по режимам |
| ConfidenceReliability | Надёжность уверенности | Корреляция conviction↔результат |
| ExecutionQuality | Качество исполнения | Дрифт execution costs |
| AdverseExcursion | Неблагоприятное отклонение | Максимальные просадки |

## Метрика деградации

Для каждого измерения рассчитывается:
```
DecayMetric {
    current_value    — текущее значение (короткое окно)
    baseline_value   — базовое значение (длинное окно)
    drift_pct        — процент отклонения от базы
    z_score          — нормализованное отклонение
    is_degraded      — превышен ли порог
}
```

## Рекомендации при деградации

В порядке возрастания серьёзности:

1. **NoAction** — без действий, стратегия здорова
2. **ReduceWeight** — снизить вес стратегии в аллокации
3. **ReduceSize** — уменьшить размер позиций
4. **RaiseThresholds** — поднять пороги входа (выше conviction)
5. **MoveToShadow** — перевести в теневой режим
6. **Disable** — полностью отключить стратегию
7. **AlertOperator** — уведомить оператора

## Логика определения здоровья

```
overall_health = 1.0 - max(drift_pct по всем измерениям)

health > 0.7         → NoAction
health 0.5..0.7      → ReduceWeight / ReduceSize
health 0.3..0.5      → RaiseThresholds / AlertOperator
health < 0.3         → MoveToShadow / Disable
```

## Конфигурация

```cpp
DecayConfig {
    short_lookback = 20,           // Короткое окно (сделки)
    long_lookback = 100,           // Длинное окно
    expectancy_drift_threshold = 0.3,   // 30% → alert
    hit_rate_drift_threshold = 0.15,    // 15% → alert
    z_score_alert_threshold = 2.0,      // z > 2 → alert
    health_critical_threshold = 0.3,    // < 0.3 → disable
    health_warning_threshold = 0.5      // < 0.5 → reduce
};
```

## Интеграция

```
TradeOutcome → record_trade_outcome()
                      ↓
              Скользящее окно per-strategy
                      ↓
              analyze() → AlphaDecayReport
                      ↓
              Рекомендация → StrategyAllocator / OperatorAlert
```
