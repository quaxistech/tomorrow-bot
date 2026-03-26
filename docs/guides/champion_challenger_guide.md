# Руководство по Champion-Challenger (v2)

## Обзор

`ChampionChallengerEngine` реализует профессиональное A/B тестирование стратегий с учётом
**реальных издержек** (комиссии + проскальзывание), multi-критериальным pre-promotion
аудитом, экспортом метрик в Prometheus и персистентностью через storage adapter.

В отличие от наивного сравнения P&L, v2 сравнивает стратегии по **net P&L** после всех
торговых издержек и требует от challenger-а пройти 4 критерия перед повышением.

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
| Promoted | Повышен — challenger превзошёл champion-а |
| Rejected | Отклонён — не прошёл аудит или уступает champion-у |
| Retired | Снят с оценки вручную |

## Структура PnLBreakdown

Все результаты сделок передаются через `PnLBreakdown` — честное разложение P&L по издержкам:

```cpp
PnLBreakdown {
    double gross_pnl_bps;    // Валовый P&L до издержек (bps)
    double fee_bps;           // Биржевая комиссия (maker=2bps, taker=6bps)
    double slippage_bps;      // Проскальзывание при исполнении (bps)

    double net_pnl_bps() const { return gross_pnl_bps - fee_bps - slippage_bps; }
};
```

Все внутренние сравнения используют `net_pnl_bps()`.

## Метрики ComparisonMetrics

| Метрика | Описание |
|---------|----------|
| `net_pnl_bps` | Чистый P&L после всех издержек (bps) |
| `gross_pnl_bps` | Валовый P&L до издержек (bps) |
| `total_fee_bps` | Накопленные биржевые комиссии (bps) |
| `total_slippage_bps` | Накопленное проскальзывание (bps) |
| `decision_count` | Количество записанных сделок |
| `profitable_count` | Прибыльных сделок (net > 0) |
| `hit_rate()` | Доля прибыльных сделок [0, 1] |
| `avg_conviction` | Средняя уверенность (Welford online mean) |
| `max_drawdown_bps` | Макс. просадка от пика (отрицательное, bps) |
| `regime_pnl` | Net P&L с разбивкой по режимам рынка |
| `regime_count` | Кол-во сделок по каждому режиму |

## Pre-Promotion Audit

Перед промоушеном challenger должен пройти 4 проверки (`PrePromotionAudit`):

```
1. pnl_delta_passed       → net_pnl_delta ≥ promotion_threshold (+20%)
2. hit_rate_adequate      → hit_rate ≥ min_hit_rate (default 0.45)
3. max_drawdown_acceptable → max_drawdown_bps ≥ max_drawdown_bps config (-500)
4. regime_consistency_passed → выигрывает в ≥60% режимов с достаточно данных
```

Для отклонения (`should_reject`) достаточно одного условия: `delta ≤ rejection_threshold`.

## Конфигурация

```cpp
ChampionChallengerConfig {
    int    min_evaluation_trades = 50;    // Мин. сделок для оценки (у обоих)
    double promotion_threshold   = 0.20; // Net delta +20% → promote
    double rejection_threshold   = -0.10;// Net delta -10% → reject
    double min_hit_rate          = 0.45; // Hit rate ≥ 45% — минимум для spot
    double max_drawdown_bps      = -500.0;// Просадка не хуже -500bps
    int    min_regime_samples    = 5;    // Мин. сделок на режим для consistency
};
```

## API

```cpp
// Инициализация с зависимостями (logging, metrics, persistence)
ChampionChallengerEngine engine(config, logger, metrics, storage);

// Регистрация пары стратегий
engine.register_challenger(champion_id, challenger_id, version);

// Запись результатов сделок
engine.record_champion_outcome(champion_id, PnLBreakdown{gross, fee, slip}, regime);
engine.record_challenger_outcome(challenger_id, breakdown, regime, conviction);

// Аудит и проверка
auto audit = engine.audit_challenger(challenger_id);  // Без промоушена
bool promote_ok = engine.should_promote(challenger_id); // С full аудитом
bool reject_ok  = engine.should_reject(challenger_id);

// Действия
engine.promote(challenger_id);   // Запускает аудит, затем повышает
engine.reject(challenger_id);    // Отклоняет

// Отчёт по champion-стратегии
auto report = engine.evaluate(champion_id);  // → ChampionChallengerReport

// Observer pattern для оповещений о promote/reject
engine.add_observer(std::make_shared<MyObserver>());
```

## Интеграция в Pipeline

`ChampionChallengerEngine` автоматически инициализируется в `TradingPipeline` и
подключён к реальному торговому потоку:

### Зарегистрированные пары стратегий

| Champion | Challenger |
|----------|------------|
| `momentum` | `mean_reversion` |
| `breakout` | `vol_expansion` |

`microstructure_scalp` работает вне C/C (слишком короткий горизонт).

### Поток данных

```
Closed trade
    ↓
record_trade_for_decay()
    ↓
PnLBreakdown = {
    gross_pnl_bps = pnl / capital × 10000,
    fee_bps       = 2 (maker) / 4 (hybrid) / 6 (taker) — из exec_alpha,
    slippage_bps  = current_position_slippage_bps_
}
    ↓
cc_engine_->record_champion_outcome()   (для momentum, breakout)
cc_engine_->record_challenger_outcome() (для mean_reversion, vol_expansion)
    ↓
check_champion_challenger_status() (каждые 5 минут)
    → should_promote / should_reject
    → promote / reject + log + observer callbacks
```

### Пример лога при промоушене

```
[CC] Challenger готов к промоушену  {champion: momentum, challenger: mean_reversion}
[CC] Сводка C/C  {chall_net_pnl: 420.5, chall_hitrate: 0.58, chall_dd: -95.0, trades: 67, status: Promoted}
```

## Observability

### Prometheus метрики

| Метрика | Тип | Описание |
|---------|-----|----------|
| `cc_challenger_net_pnl_bps` | gauge | Net P&L challenger-а (bps) |
| `cc_challenger_hit_rate` | gauge | Hit rate [0, 1] |
| `cc_challenger_drawdown_bps` | gauge | Макс. просадка (bps) |
| `cc_challenger_trade_count` | gauge | Количество сделок |
| `cc_promotions_total` | counter | Суммарно промоушенов |
| `cc_rejections_total` | counter | Суммарно отклонений |

Метрики экспортируются каждые 10 решений (не на каждый тик).

### Persistence

Все события (promotion, rejection, outcome) записываются в journal через
`IStorageAdapter::append_journal()` с payload-префиксом `cc:`:

```json
{"type": "cc:promote", "champion": "momentum", "challenger": "mean_reversion",
 "net_pnl_delta": 0.42, "hit_rate": 0.58, "drawdown_bps": -95.0}
```

## Связь с AlphaDecayMonitor

Оба модуля получают данные из одного хука `record_trade_for_decay()`:

| Модуль | Отслеживает | Действие |
|--------|-------------|---------|
| AlphaDecayMonitor | Деградацию сигнала | Снижает размер позиции |
| ChampionChallengerEngine | Сравнительную эффективность | Рекомендует promote/reject |

Используйте оба модуля совместно для полноценного мониторинга здоровья стратегий.

## Реализация IChallengerObserver

```cpp
class MyAlertObserver : public champion_challenger::IChallengerObserver {
public:
    void on_promotion(const ChallengerEntry& entry) override {
        send_alert("Стратегия " + entry.challenger_id.get() + " повышена!");
    }
    void on_rejection(const ChallengerEntry& entry) override {
        send_alert("Стратегия " + entry.challenger_id.get() + " отклонена: "
                   + entry.rejection_reason);
    }
};

engine.add_observer(std::make_shared<MyAlertObserver>());
```
