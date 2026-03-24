# Схема телеметрии (TelemetryEnvelope)

## Обзор

ResearchTelemetry — структурированная система сбора телеметрии, охватывающая
весь pipeline обработки: от рыночных данных до исполнения. Каждый конверт (envelope)
содержит полный контекст решения для пост-анализа и исследований.

## Структура TelemetryEnvelope

### Идентификация
| Поле | Тип | Описание |
|------|-----|----------|
| sequence_id | uint64_t | Монотонный ID конверта |
| correlation_id | CorrelationId | ID корреляции запроса |
| captured_at | Timestamp | Время захвата (наносекунды) |
| symbol | Symbol | Торговый инструмент |

### Атрибуция конфигурации
| Поле | Тип | Описание |
|------|-----|----------|
| strategy_id | StrategyId | ID стратегии |
| strategy_version | StrategyVersion | Версия стратегии |
| config_hash | ConfigHash | SHA-256 хэш конфигурации |

### Рыночный контекст
| Поле | Тип | Описание |
|------|-----|----------|
| last_price | Price | Последняя цена |
| mid_price | Price | Средняя цена |
| spread_bps | double | Спред (базисные пункты) |

### Feature Snapshot
| Поле | Тип | Описание |
|------|-----|----------|
| features_json | string | Сериализованный FeatureSnapshot (JSON) |

### Состояние мира / Режим / Неопределённость
| Поле | Тип | Описание |
|------|-----|----------|
| world_state | string | Состояние мира (WorldState) |
| regime_label | string | Режим рынка (DetailedRegime) |
| regime_confidence | double | Уверенность в классификации [0,1] |
| uncertainty_level | string | Уровень неопределённости |
| uncertainty_score | double | Агрегированный скор [0,1] |

### Стратегия и решение
| Поле | Тип | Описание |
|------|-----|----------|
| strategy_proposals_json | string | JSON предложений стратегий |
| allocation_result_json | string | JSON результата аллокации |
| decision_json | string | JSON финального решения |
| trade_approved | bool | Одобрена ли сделка |
| final_conviction | double | Финальная уверенность [0,1] |

### Риск
| Поле | Тип | Описание |
|------|-----|----------|
| risk_verdict | string | Вердикт риска (Approved/Denied/...) |
| risk_reasons_json | string | JSON причин риска |

### Исполнение
| Поле | Тип | Описание |
|------|-----|----------|
| execution_style | string | Стиль исполнения |
| execution_urgency | double | Срочность [0,1] |
| execution_cost_bps | double | Стоимость исполнения (bps) |

### Портфель
| Поле | Тип | Описание |
|------|-----|----------|
| portfolio_exposure_pct | double | Экспозиция (% капитала) |
| daily_pnl | double | P&L за день (USD) |
| drawdown_pct | double | Просадка (%) |
| open_positions | int | Открытые позиции |

### Латентность
| Поле | Тип | Описание |
|------|-----|----------|
| latency_traces | vector | Трассировки латентности по этапам |
| total_pipeline_ns | int64_t | Общее время pipeline (нс) |

### Исход (заполняется позже)
| Поле | Тип | Описание |
|------|-----|----------|
| realized_pnl | optional<double> | Реализованный P&L |
| slippage_bps | optional<double> | Проскальзывание (bps) |

## Назначение телеметрии

- **Пост-торговый анализ** — разбор каждой сделки
- **Исследование альфы** — поиск закономерностей
- **Исследование исполнения** — анализ качества исполнения
- **Сравнение challenger'ов** — A/B тестирование
- **Детекция дрифта** — обнаружение деградации
- **Диагностика** — отладка проблем

## Архитектура приёмников (Sinks)

```
ResearchTelemetry → ITelemetrySink (интерфейс)
                      ├── MemoryTelemetrySink (для тестов)
                      ├── FileTelemetrySink (будущее)
                      └── DatabaseTelemetrySink (будущее)
```
