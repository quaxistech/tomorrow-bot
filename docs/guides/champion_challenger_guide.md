# Руководство по Champion-Challenger

## Обзор

ChampionChallengerEngine реализует A/B тестирование стратегий: действующая стратегия
(champion) торгует в live-режиме, а одна или несколько альтернативных стратегий
(challengers) работают виртуально. Система сравнивает их на структурированных метриках
и поддерживает логику повышения/отклонения.

## Жизненный цикл Challenger'а

```
Registered → Evaluating → Promoted
                       → Rejected
                       → Retired
```

| Статус | Описание |
|--------|----------|
| Registered | Зарегистрирован, ожидает начала оценки |
| Evaluating | В процессе оценки (после первых результатов) |
| Promoted | Повышен до champion'а |
| Rejected | Отклонён (недостаточная производительность) |
| Retired | Снят с оценки вручную |

## Измерения сравнения

| Метрика | Описание |
|---------|----------|
| hypothetical_pnl_bps | Гипотетический P&L в базисных пунктах |
| signal_quality | Качество сигналов [0, 1] (hit_rate) |
| decision_count | Количество решений |
| avg_conviction | Средняя уверенность |
| regime_pnl | P&L с разбивкой по режимам |
| execution_quality | Качество исполнения |

## Логика повышения и отклонения

```
relative_performance = (challenger_pnl - champion_pnl) / |champion_pnl|

Если relative_performance > promotion_threshold (+20%):
    → should_promote = true

Если relative_performance < rejection_threshold (-10%):
    → should_reject = true

Иначе:
    → Продолжить оценку
```

**Минимальное количество сделок**: по умолчанию 50 для обоих участников.
При недостаточных данных ни повышение, ни отклонение не происходят.

## Конфигурация

```cpp
ChampionChallengerConfig {
    min_evaluation_trades = 50,     // Мин. сделок для оценки
    promotion_threshold = 0.2,      // +20% → promote
    rejection_threshold = -0.1      // -10% → reject
};
```

## Интерфейс

```cpp
// Регистрация challenger'а
register_challenger(champion_id, challenger_id, version)

// Запись результатов
record_champion_outcome(champion_id, pnl_bps, regime)
record_challenger_outcome(challenger_id, pnl_bps, regime, conviction)

// Оценка
evaluate(champion_id) → ChampionChallengerReport
should_promote(challenger_id) → bool
should_reject(challenger_id) → bool

// Действия
promote(challenger_id) → VoidResult
reject(challenger_id) → VoidResult
```

## Интеграция с Shadow Mode

Champion-Challenger естественно дополняет Shadow Mode:
1. Challenger стратегия работает через ShadowModeEngine
2. Решения записываются в обе системы
3. ChampionChallengerEngine сравнивает агрегированные метрики
4. AlphaDecayMonitor отслеживает здоровье обоих участников
