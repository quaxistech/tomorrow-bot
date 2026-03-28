# Staged Pipeline Architecture v2.0

Документ описывает архитектуру staged pipeline, введённую в версии 2.0.
Каждый тик проходит строго определённые стадии с SLA-бюджетами и типизированными результатами.

---

## Стадии pipeline

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                           Trading Pipeline (on_feature_snapshot)                 │
│                                                                                  │
│  ┌─────────────────┐    ┌────────────────────┐    ┌──────────────────────────┐  │
│  │  Stage 0:        │    │  Stage 1:           │    │  Stage 2:                │  │
│  │  Ingress         │───►│  Freshness Gate     │───►│  Backlog Detection       │  │
│  │  (≤100 мкс)      │    │  (≤100 мкс)         │    │  (≤100 мкс)              │  │
│  └─────────────────┘    └────────────────────┘    └──────────────────────────┘  │
│                                                                  │               │
│  ┌──────────────────────────────────────────────────────────────▼─────────────┐  │
│  │  Stage 3: Periodic Tasks (watchdog + reconciliation — фоновые задачи)      │  │
│  └──────────────────────────────────────────────────────────────┬─────────────┘  │
│                                                                  │               │
│  ┌─────────────────┐    ┌────────────────────┐    ┌─────────────▼────────────┐  │
│  │  Stage 4:        │    │  Stage 5:           │    │  Stage 6:                │  │
│  │  ML Signals      │◄───│  Market Context     │◄───│  Feature Update          │  │
│  │  (≤1000 мкс)     │    │  (≤500 мкс)         │    │                          │  │
│  └────────┬────────┘    └────────────────────┘    └──────────────────────────┘  │
│           │                                                                      │
│  ┌────────▼────────┐    ┌────────────────────┐    ┌──────────────────────────┐  │
│  │  Stage 7:        │    │  Stage 8:           │    │  Stage 9:                │  │
│  │  Strategy        │───►│  Decision + Filters │───►│  Risk Engine             │  │
│  │  Signals         │    │  (≤500 мкс)         │    │  (≤300 мкс)              │  │
│  │  (≤2000 мкс)     │    │                     │    │                          │  │
│  └─────────────────┘    └────────────────────┘    └──────────────────────────┘  │
│                                                                  │               │
│  ┌──────────────────────────────────────────────────────────────▼─────────────┐  │
│  │  Stage 10: Execution (Exec Alpha + Order Submit)  (≤1000 мкс)             │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                                                                                  │
│  ════════════════════════════════════════════════════════════════════════════    │
│  Суммарный бюджет: ≤5000 мкс (5 мс) на полный цикл тика                         │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## SLA-бюджеты по стадиям

| Стадия | Константа | Бюджет | Описание |
|--------|-----------|--------|---------|
| Ingress | `kBudgetIngressUs` | 100 мкс | Получение тика, freshness gate |
| ML Signals | `kBudgetMlUs` | 1 000 мкс | Энтропия, каскады, корреляции, Thompson |
| Market Context | `kBudgetContextUs` | 500 мкс | World Model + Regime + Uncertainty |
| Strategy Signals | `kBudgetSignalUs` | 2 000 мкс | Все стратегии + allocator |
| Decision + Filters | `kBudgetDecisionUs` | 500 мкс | Decision Engine + adversarial veto |
| Risk Engine | `kBudgetRiskUs` | 300 мкс | 14 жёстких проверок |
| Execution | `kBudgetExecUs` | 1 000 мкс | Exec Alpha + отправка ордера |
| **Total** | `kBudgetTotalUs` | **5 000 мкс** | Полный цикл тика |

---

## PipelineTickContext

`PipelineTickContext` — единая структура-носитель, передаваемая через все стадии.

```cpp
struct PipelineTickContext {
    // Входные данные
    features::FeatureSnapshot snapshot;
    int64_t ingress_ns;
    uint64_t tick_sequence;

    // Phase 1: Freshness Gate
    FreshnessResult freshness;

    // Аналитические стадии
    std::optional<world_model::WorldModelSnapshot> world;
    std::optional<regime::RegimeSnapshot>          regime;
    std::optional<uncertainty::UncertaintySnapshot> uncertainty;
    std::optional<adversarial::DefenseAssessment>  adversarial;
    std::optional<ml::MlSignalSnapshot>            ml_signals;

    // Стратегические сигналы
    std::vector<strategy::TradeIntent> raw_intents;
    std::vector<strategy::TradeIntent> filtered_intents;

    // Решение и исполнение
    std::optional<decision::DecisionRecord>             decision;
    std::optional<strategy::TradeIntent>                approved_intent;
    std::optional<risk::RiskDecision>                   risk;
    std::optional<execution_alpha::ExecutionAlphaResult> exec_alpha;

    // Отказы
    std::vector<std::string> veto_reasons;
    bool traded{false};

    // Латентность по стадиям
    std::vector<StageLatency> stage_latencies;
};
```

### Вспомогательные методы

| Метод | Описание |
|-------|---------|
| `begin_stage()` | Возвращает монотонное время (нс) старта стадии |
| `end_stage(name, start_ns)` | Записывает длительность стадии в `stage_latencies` |
| `total_latency_us()` | Суммарная латентность по всем стадиям |

---

## PipelineStageResult

Типизированный результат выполнения одной стадии.

```cpp
enum class StageOutcome : uint8_t {
    Pass,     // Стадия пройдена, продолжаем
    Veto,     // Жёсткий отказ — тик отклонён
    Degrade,  // Мягкий отказ — продолжаем с ограничениями
    Escalate  // Требуется вмешательство оператора
};
```

**Фабричные функции:**
- `stage_pass(name, dur_us)` — успешное прохождение
- `stage_veto(name, reason, dur_us)` — блокировка тика
- `stage_degrade(name, reason, dur_us)` — деградированное состояние
- `stage_escalate(name, reason, dur_us)` — эскалация

---

## Phase 1: Freshness Gate

Первая защита горячего пути — отклонение устаревших котировок.

**Алгоритм:**
1. Если `snapshot.computed_at <= 0` — тик считается свежим (неизвестное время)
2. Вычислить `age_ns = now - snapshot.computed_at`
3. Если `age_ns > 5'000'000'000` (5 секунд) — вернуть, инкрементировать `pipeline_stale_ticks_total`
4. Каждые 50 отклонённых тиков — логировать предупреждение

**Почему это важно:** устаревшие котировки могут вызвать сигналы на неактуальных ценах, что приводит к плохим fills и потерям.

---

## Phase 1: Backlog Detection

Мониторинг gap между последовательными тиками.

**Алгоритм:**
1. Сохраняем `last_tick_ingress_ns` в `thread_local` переменной
2. При каждом тике вычисляем `gap_ns = now - last_tick_ingress_ns`
3. Если `gap_ns > 2'000'000'000` (2 секунды) и тиков > `kMinWarmupTicks`:
   - Логировать предупреждение
   - Инкрементировать `pipeline_tick_gaps_total`

**Диагностика:** большой gap означает, что предыдущий тик обрабатывался слишком долго (pipeline не успевает) или WebSocket соединение было прервано.

---

## Phase 1: PipelineLatencyTracker

Кольцевой буфер из 512 сэмплов для каждой стадии.

**Класс `StageTimer` (RAII):**
```cpp
// Использование:
{
    StageTimer timer(*latency_tracker_, "ml_signals", kBudgetMlUs);
    // ... выполнение стадии ...
}  // в деструкторе: record + check_sla
```

**Метрики (Prometheus gauges):**
- `pipeline_stage_latency_p50_us{stage="..."}`
- `pipeline_stage_latency_p95_us{stage="..."}`
- `pipeline_stage_latency_p99_us{stage="..."}`
- `pipeline_stage_latency_max_us{stage="..."}`
- `pipeline_stage_latency_avg_us{stage="..."}`
- `pipeline_stage_sla_violations{stage="..."}`

---

## Phase 2: Order Watchdog

Периодическая проверка (каждые 10 секунд) всех активных ордеров.

### Матрица классификации

| Состояние ордера | Timeout | Действие |
|-----------------|---------|---------|
| `PendingAck` | 5 сек | Cancel |
| `Open` | 30 сек | Cancel |
| `PartiallyFilled` | 60 сек | ForceClose |
| `UnknownRecovery` | 10 сек | RecoverState |
| Остальные | — | Ok |

### Конфигурация (OrderWatchdogConfig)

```yaml
max_pending_ack_ms: 5000
max_open_order_ms: 30000
max_partial_fill_ms: 60000
max_unknown_recovery_ms: 10000
check_interval_ms: 10000
```

### Метрики

| Метрика | Описание |
|--------|---------|
| `pipeline_watchdog_stale_cancelled` | Отменённые зависшие ордера |
| `pipeline_watchdog_unknown_recovery` | Обнаруженные UnknownRecovery ордера |
| `pipeline_watchdog_partial_fill_timeout` | PartiallyFilled timeout |

### Callbacks

- **cancel_callback**: вызывается при автоматической отмене ордера
- **alert_callback**: вызывается для RecoverState (эскалация оператору)

---

## Phase 4: Continuous Reconciliation

Сравнение внутреннего состояния с биржей каждые 60 секунд.

### BitgetExchangeQueryAdapter

Реализует `reconciliation::IExchangeQueryService` поверх `BitgetRestClient`.

| Метод | Эндпоинт Bitget | Описание |
|-------|----------------|---------|
| `get_open_orders(symbol)` | `GET /api/v2/spot/trade/unfilled-orders` | Получить активные ордера |
| `get_account_balances()` | `GET /api/v2/spot/account/assets` | Получить балансы ассетов |
| `get_order_status(id, symbol)` | `GET /api/v2/spot/trade/orderInfo` | Статус конкретного ордера |

### Алгоритм reconciliation

1. Каждые 60 секунд (если `reconciliation_engine_` инициализирован)
2. Получить `execution_engine_->active_orders()` — локальные активные ордера
3. Если нет активных ордеров — пропустить (ноль сетевых запросов)
4. Вызвать `reconciliation_engine_->reconcile_active_orders(active_orders)`
5. Если обнаружены расхождения — логировать `warn("reconciliation", ...)`
6. Каждые 500 тиков при отсутствии расхождений — логировать `debug("reconciliation", "OK")`

### Типы расхождений (MismatchType)

| Тип | Описание |
|-----|---------|
| `OrderExistsOnlyLocally` | Ордер есть в системе, нет на бирже |
| `OrderExistsOnlyOnExchange` | Ордер на бирже, нет в системе |
| `StateMismatch` | Разное состояние (биржа=Filled, система=Open) |
| `QuantityMismatch` | Расходится filled_qty |
| `BalanceMismatch` | Баланс выходит за пределы допуска |

---

## Ссылки

- [Архитектурный обзор](overview.md)
- [Поток исполнения](execution_flow.md)
- [Production Hardening](production_hardening.md)
- `src/pipeline/pipeline_tick_context.hpp`
- `src/pipeline/pipeline_stage_result.hpp`
- `src/pipeline/pipeline_latency_tracker.hpp`
- `src/pipeline/order_watchdog.hpp`
- `src/exchange/bitget/bitget_exchange_query_adapter.hpp`
