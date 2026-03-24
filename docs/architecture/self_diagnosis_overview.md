# Обзор самодиагностики (SelfDiagnosisEngine)

## Обзор

SelfDiagnosisEngine — движок объяснения решений системы. Генерирует структурированные
диагностические записи, которые объясняют: почему сделка была совершена, почему отклонена,
какое состояние системы привело к решению.

## Типы диагностики

| Тип | Описание |
|-----|----------|
| TradeTaken | Объяснение совершённой сделки |
| TradeDenied | Объяснение отказа от сделки |
| SystemState | Диагностика текущего состояния системы |
| StrategyHealth | Здоровье конкретной стратегии |
| DegradedState | Диагностика деградированного состояния |

## Диагностическая запись (DiagnosticRecord)

```cpp
DiagnosticRecord {
    diagnostic_id,       // Уникальный ID (атомарный счётчик)
    type,                // Тип диагностики
    correlation_id,      // ID корреляции
    symbol,              // Торговый инструмент
    created_at,          // Время создания
    
    // Контекст
    world_state,         // Состояние мира
    regime,              // Режим рынка
    uncertainty_level,   // Уровень неопределённости
    
    // Факторы решения
    factors[],           // Список факторов с impact [-1, +1]
    
    // Вердикт
    verdict,             // Краткий вердикт
    human_summary,       // Человекочитаемое объяснение
    machine_json,        // Машиночитаемый JSON
    
    // Метаданные
    strategy_id,
    risk_verdict,
    trade_executed
};
```

## Факторы решения (DiagnosticFactor)

Каждый фактор содержит:
- **component** — компонент системы (world_model, risk, strategy:momentum и т.д.)
- **observation** — текстовое наблюдение
- **impact** — влияние на решение [-1 = категорически против, +1 = категорически за]

### Пример факторов для совершённой сделки:
```
[strategy:momentum]  "Сигнал с уверенностью 0.85"          impact: +0.85
[world_model]        "Состояние: StableTrendContinuation"   impact: +0.7
[regime]             "Режим: StrongUptrend"                 impact: +0.6
[risk]               "Одобрено, утилизация риска: 35%"      impact: +0.5
[uncertainty]        "Уровень: Moderate"                    impact: -0.2
```

### Пример факторов для отклонённой сделки:
```
[risk]               "Отклонено: MAX_DAILY_LOSS"           impact: -1.0
[risk]               "Kill switch активен"                  impact: -1.0
[strategy:momentum]  "Сигнал с уверенностью 0.45"          impact: +0.45
```

## Объяснение совершённой сделки (explain_trade)

Входные данные:
- DecisionRecord — решение агрегатора
- RiskDecision — вердикт риска
- Контекст: world_state, regime, uncertainty_level

Логика:
1. Извлечь вклады стратегий из decision.contributions
2. Добавить факторы мира, режима, неопределённости
3. Добавить факторы риска (одобрено / размер уменьшен)
4. Сгенерировать человекочитаемое резюме
5. Сгенерировать машиночитаемый JSON

## Объяснение отказа (explain_denial)

1. Извлечь глобальные вето из decision.global_vetoes
2. Извлечь вето стратегий из contributions[].veto_reasons
3. Извлечь причины риска из risk_decision.reasons
4. Сгенерировать объяснение отказа

## Диагностика состояния системы

1. Оценить состояние мира (Disrupted = негативный фактор)
2. Оценить режим (LiquidityStress, ToxicFlow = опасные)
3. Оценить неопределённость (Extreme = блокировка торговли)
4. Проверить портфель (высокая просадка = деградация)
5. Проверить kill switch

## Форматы вывода

### Человекочитаемое резюме
```
Сделка совершена по стратегии [momentum_v2], уверенность 85%.
Режим: StrongUptrend. Мир: StableTrendContinuation. 
Неопределённость: Moderate. Риск: одобрено.
Факторов «за»: 4, «против»: 1.
```

### Машиночитаемый JSON
```json
{
    "diagnostic_id": 42,
    "type": "TradeTaken",
    "symbol": "BTCUSDT",
    "world_state": "StableTrendContinuation",
    "regime": "StrongUptrend",
    "factors": [
        {"component": "strategy:momentum", "impact": 0.85},
        {"component": "risk", "impact": 0.5}
    ],
    "verdict": "Сделка одобрена"
}
```
