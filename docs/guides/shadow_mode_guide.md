# Shadow Mode v2.0 — Руководство по подсистеме виртуального исполнения

Профессиональная подсистема виртуального исполнения торговых решений.
Записывает гипотетические сделки, симулирует fill quality с учётом комиссий и slippage,
отслеживает P&L в конфигурируемых окнах, ведёт shadow-позиции и формирует
аналитику расхождений с live-результатами. Реальные ордера **никогда** не отправляются.

---

## 1. Гарантия безопасности

**КРИТИЧЕСКИ ВАЖНО**: shadow-подсистема архитектурно изолирована от исполнения.

- `ShadowModeEngine` **не имеет доступа** к `IOrderSubmitter`
- Все решения записываются как `ShadowDecision` (read-only структура)
- Нет code path из shadow-контура в реальный execution pipeline
- При активном kill switch подсистема прекращает запись (`respect_kill_switch`)

Конструктор `ShadowModeEngine` принимает `ILogger`, `IClock`, `IMetricsRegistry`,
`GovernanceAuditLayer`, `IStorageAdapter` — ни один из них не способен отправить ордер.

---

## 2. Архитектура

### Компоненты

| Файл | Назначение |
|------|-----------|
| `shadow_types.hpp` | 4 enum (`ShadowMode`, `ShadowRiskPolicy`, `SignalIntent`, `ShadowOrderState`) + 10 structs |
| `shadow_mode_engine.hpp/cpp` | ~640 LOC — ядро: fill simulation, price tracking, position tracking, alerts, metrics, persistence |

### Интеграция в TradingPipeline

`TradingPipeline` конструктор:
1. Создаёт `ShadowModeEngine` из `AppConfig::shadow`
2. При `persist_to_db == true` передаёт `IStorageAdapter` (PostgreSQL через `POSTGRES_URL`)
3. Передаёт `GovernanceAuditLayer` для аудита

Каждый тик:
- `update_price_tracking(symbol, price, now)` — обновление ценовых окон
- `cleanup_stale_records(now)` — вытеснение просроченных записей

После решения risk-модуля:
- `record_decision(ShadowDecision)` — фиксация решения с полным контекстом

### Зависимости

```
ShadowModeEngine
  ├── ILogger           — структурированное логирование
  ├── IClock            — абстракция времени (наносекундная точность)
  ├── IMetricsRegistry  — Prometheus counters/gauges (опционально)
  ├── GovernanceAuditLayer — аудит-события (опционально)
  └── IStorageAdapter   — PostgreSQL persistence (опционально)
```

Все опциональные зависимости принимаются как `nullptr` и безопасно пропускаются.

---

## 3. Режимы работы

| Режим | `ShadowMode` | Описание |
|-------|-------------|----------|
| **Observation** | `Observation` | Чистая запись — все сигналы фиксируются as-is без вмешательства |
| **Validation** | `Validation` | Дублирование live-pipeline — shadow vs live сравнение в реальном времени |
| **Discovery** | `Discovery` | Scenario exploration — альтернативные risk/execution параметры |

Режим задаётся в конфигурации (`shadow.mode`) и определяет контекст записи.

---

## 4. Risk Policies

| Политика | `ShadowRiskPolicy` | Применение |
|----------|-------------------|-----------|
| **MirrorLive** | `MirrorLive` | Shadow повторяет live risk limits. Для validation |
| **Relaxed** | `Relaxed` | Ослабленные лимиты. Для исследования краёв |
| **Unconstrained** | `Unconstrained` | Без risk limits. Для «что если» анализа |

---

## 5. Жизненный цикл shadow-решения

```
1. Стратегия генерирует сигнал → DecisionAggregation → RiskEngine вердикт
2. record_decision(ShadowDecision) — фиксация решения с контекстом:
   • correlation_id, strategy_id, symbol, side, signal_intent
   • conviction, intended_price, quantity
   • world_state, regime, uncertainty_level, risk_verdict
   • feature_snapshot_json, risk_decision_json
   • would_have_been_live (прошёл бы risk или нет)
3. simulate_fill() → ShadowFillSimulation:
   • simulated_fill_price, estimated_slippage_bps
   • entry_fee_bps, exit_fee_bps, order_state
4. ShadowTradeRecord создан с market_price_at_decision
5. update_price_tracking() на каждом тике:
   • price_at_short (t + eval_window_short)
   • price_at_mid (t + eval_window_mid)
   • price_at_long (t + eval_window_long)
   • had_data_gap = true при пропуске данных
6. compute_gross_pnl_bps() → gross P&L
   compute_net_pnl_bps()  → net P&L (gross - fees)
7. update_position() — обновление shadow-позиции
8. export_metrics() — Prometheus counters
9. persist_record() — PostgreSQL (если включено)
10. tracking_complete = true, completed_at заполняется
```

---

## 6. Fill Simulation

`ShadowFillSimulation` моделирует качество исполнения:

| Поле | Тип | Описание |
|------|-----|----------|
| `simulated_fill_price` | `Price` | Цена исполнения с учётом slippage |
| `estimated_slippage_bps` | `double` | Оценка проскальзывания (bps) |
| `entry_fee_bps` | `double` | Комиссия входа (bps) |
| `exit_fee_bps` | `double` | Комиссия выхода (bps) |
| `order_state` | `ShadowOrderState` | Lifecycle: Pending → Submitted → Filled / Cancelled / Expired |

Комиссии берутся из конфигурации:
- `taker_fee_pct` — комиссия taker (по умолчанию 0.1%)
- `maker_fee_pct` — комиссия maker (по умолчанию 0.08%)

Net P&L = gross P&L - entry_fee - exit_fee.

---

## 7. Position Tracking

Shadow-позиции агрегируются по ключу `strategy_id:symbol`.

### ShadowPositionLeg

Каждый вход/выход — отдельная «нога»:

```cpp
ShadowPositionLeg {
    symbol, side, quantity, fill_price, fee_bps, timestamp
};
```

### ShadowPosition

```cpp
ShadowPosition {
    symbol, strategy_id,
    entry_legs[],          // Вектор входных ног
    exit_leg,              // Опциональная нога выхода
    total_entry_notional,  // Суммарный notional входа
    weighted_entry_price,  // Средневзвешенная цена входа
    unrealized_pnl_bps,    // Нереализованный P&L
    realized_pnl_bps,      // Реализованный P&L
    is_open, opened_at, closed_at
};
```

Multi-leg поддержка: позиция может наращиваться несколькими `LongEntry` перед
единственным `LongExit` / `Flatten`. Средневзвешенная цена пересчитывается на каждом leg.

Signal intents:
- `LongEntry` — открытие / добавление к позиции
- `LongExit` — полное закрытие
- `ReducePosition` — частичное закрытие
- `Flatten` — полное закрытие всех позиций

---

## 8. Price Tracking

### Конфигурируемые окна оценки

| Окно | Параметр (YAML / config) | По умолчанию | Назначение |
|------|-------------------------|-------------|-----------|
| Short | `eval_window_short_ms` / `eval_window_short_ns` | 1 с | Краткосрочное движение, slippage proxy |
| Mid | `eval_window_mid_ms` / `eval_window_mid_ns` | 5 с | Среднесрочная динамика |
| Long | `eval_window_long_ms` / `eval_window_long_ns` | 30 с | P&L proxy, основная метрика |

### ShadowPriceSnapshot

```cpp
ShadowPriceSnapshot {
    price_at_short,   // optional<Price> — через ~1 с
    price_at_mid,     // optional<Price> — через ~5 с
    price_at_long,    // optional<Price> — через ~30 с
    is_complete,      // true когда все окна заполнены
    had_data_gap,     // true при пропуске рыночных данных
    last_update       // Timestamp последнего обновления
};
```

### Вытеснение просроченных записей

Если запись не получила все три price snapshot за `stale_record_timeout_ns`
(по умолчанию 120 с), она помечается как stale и вытесняется при
`cleanup_stale_records()`. Счётчик `shadow_data_gaps_total` инкрементируется.

### Формула P&L (bps)

```
gross_pnl_bps = (price_at_long - intended_price) / intended_price × 10000
```

Для Sell-решений знак инвертируется. Net P&L вычитает комиссии.

---

## 9. Сравнение Shadow vs Live

`compare()` формирует `ShadowComparison`:

```cpp
ShadowComparison {
    strategy_id,
    shadow_trades,      live_trades,
    shadow_gross_pnl_bps, shadow_net_pnl_bps, live_pnl_bps,
    shadow_hit_rate,    live_hit_rate,
    pnl_correlation,
    max_drawdown_bps,
    period_start,       period_end,
    divergence_reasons[]   // Список причин расхождения
};
```

Поле `divergence_reasons` заполняется автоматически при:
- Расхождении P&L > `alert_pnl_divergence_bps`
- Расхождении hit rate > `alert_hit_rate_divergence`
- Несоответствии количества сделок

---

## 10. Метрики (Prometheus)

Экспортируются через `IMetricsRegistry`:

| Метрика | Тип | Описание |
|---------|-----|----------|
| `shadow_decisions_total` | Counter | Общее количество shadow-решений |
| `shadow_gross_pnl_bps` | Gauge | Совокупный gross P&L (bps) |
| `shadow_net_pnl_bps` | Gauge | Совокупный net P&L (bps) |
| `shadow_data_gaps_total` | Counter | Количество пропусков данных |

Метрики обновляются в `export_metrics()` при completion каждой записи.
Если `IMetricsRegistry` не предоставлен — безопасно пропускается.

---

## 11. Алерты

`check_alerts()` генерирует `ShadowAlert` при дивергенции:

| Тип алерта | Порог (config) | Severity |
|-----------|---------------|----------|
| `pnl_divergence` | `alert_pnl_divergence_bps` (default: 100 bps) | warn / critical |
| `hit_rate_divergence` | `alert_hit_rate_divergence` (default: 0.10) | warn |
| `trade_count_mismatch` | shadow_trades ≠ live_trades | info |

Структура алерта:

```cpp
ShadowAlert {
    strategy_id, alert_type, severity, message,
    detected_at, shadow_value, live_value
};
```

---

## 12. Конфигурация

### Полный пример YAML

```yaml
shadow:
  enabled: true                         # Включение подсистемы
  mode: observation                     # observation | validation | discovery
  risk_policy: mirror_live              # mirror_live | relaxed | unconstrained
  max_records_per_strategy: 10000       # Макс. записей на стратегию (FIFO eviction)
  eval_window_short_ms: 1000            # Окно короткой оценки (1 с)
  eval_window_mid_ms: 5000              # Окно средней оценки (5 с)
  eval_window_long_ms: 30000            # Окно длинной оценки (30 с)
  stale_timeout_ms: 120000              # Таймаут просроченных записей (2 мин)
  taker_fee_pct: 0.001                  # Комиссия taker (0.1%)
  maker_fee_pct: 0.0006                 # Комиссия maker (0.06%)
  persist_to_db: false                  # PostgreSQL persistence
  respect_kill_switch: true             # Остановка при kill switch
  alert_pnl_divergence_bps: 50.0        # Порог алерта P&L дивергенции (bps)
  alert_hit_rate_divergence: 0.15       # Порог алерта hit rate дивергенции
```

### Описание параметров

| Параметр | Тип | Default | Описание |
|----------|-----|---------|----------|
| `enabled` | bool | `false` | Включение shadow-подсистемы |
| `mode` | string | `observation` | Режим работы: observation / validation / discovery |
| `risk_policy` | string | `mirror_live` | Политика риска: mirror_live / relaxed / unconstrained |
| `max_records_per_strategy` | int | `10000` | Макс. записей на стратегию; при превышении — FIFO eviction |
| `eval_window_short_ms` | int | `1000` | Окно короткой оценки (мс) |
| `eval_window_mid_ms` | int | `5000` | Окно средней оценки (мс) |
| `eval_window_long_ms` | int | `30000` | Окно длинной оценки (мс) |
| `stale_timeout_ms` | int | `120000` | Таймаут для stale записей (мс) |
| `taker_fee_pct` | double | `0.001` | Комиссия taker (0.1%) |
| `maker_fee_pct` | double | `0.0008` | Комиссия maker (0.08%) |
| `persist_to_db` | bool | `false` | Включить PostgreSQL persistence |
| `respect_kill_switch` | bool | `true` | Реагировать на kill switch |
| `alert_pnl_divergence_bps` | double | `100.0` | Порог алерта P&L дивергенции (bps) |
| `alert_hit_rate_divergence` | double | `0.10` | Порог алерта hit rate дивергенции |

---

## 13. Governance и Audit

Shadow-подсистема интегрирована с `GovernanceAuditLayer`:

- **`AuditEventType::ShadowDecisionRecorded`** — при каждом `record_decision()`
- **`AuditEventType::ShadowComparisonGenerated`** — при каждом `compare()`

Все аудит-события содержат `CorrelationId` для сквозной трассировки.
Если `GovernanceAuditLayer` не предоставлен — аудит пропускается.

---

## 14. PostgreSQL Persistence

При `persist_to_db: true`:

1. Требуется переменная окружения `POSTGRES_URL`
2. `TradingPipeline` передаёт `IStorageAdapter` в конструктор `ShadowModeEngine`
3. Каждая завершённая `ShadowTradeRecord` сохраняется через `persist_record()`
4. Тип журнала: `JournalEntryType::ShadowEvent`

PostgreSQL используется для долгосрочного хранения shadow-данных,
позволяя анализ performance за произвольные периоды.

---

## 15. Operator Controls

Оператор может управлять shadow-подсистемой в runtime:

| Метод | Описание |
|-------|----------|
| `set_enabled(bool)` | Включение / отключение записи shadow-решений |
| `set_kill_switch(bool)` | Принудительная остановка при `respect_kill_switch == true` |
| `is_enabled()` | Текущее состояние подсистемы |

Интеграция с `OperatorControl`: команда `shadow` через governance control plane.

---

## 16. Применение

| Сценарий | Режим | Risk Policy | Описание |
|----------|-------|------------|----------|
| **Тестирование стратегий** | Observation | MirrorLive | Новая стратегия работает в shadow до набора доверия |
| **A/B тестирование** | Validation | MirrorLive | Параллельное сравнение shadow vs live |
| **Canary deployment** | Validation | MirrorLive | Новая версия стратегии в shadow, старая — live |
| **Pre-production validation** | Validation | MirrorLive | Прогрев перед переходом в production |
| **Research / «что если»** | Discovery | Relaxed / Unconstrained | Исследование альтернативных параметров |
| **Режим наблюдения** | Observation | MirrorLive | Система работает, но не торгует |

---

## 17. Агрегированные метрики

`get_metrics_summary()` возвращает `ShadowMetricsSummary`:

```cpp
ShadowMetricsSummary {
    total_decisions,       completed_decisions,    incomplete_decisions,
    gross_pnl_bps,         net_pnl_bps,            hit_rate,
    avg_trade_pnl_bps,     max_drawdown_bps,       sharpe_estimate,
    decisions_blocked_by_risk,  data_gap_count
};
```

Sharpe estimate и max drawdown рассчитываются по всем завершённым записям.
