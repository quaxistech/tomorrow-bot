# `src/ml` — ML-компоненты (минимальный набор)

## Назначение

Набор статистических/ML-моделей для адаптации стратегии: байесовские параметры, multi-armed bandit (Thompson), entropy filter, microstructure fingerprinting, liquidation cascade detection, multi-asset correlation.

> **Scalping refactor (2026-05):** Удалены неиспользуемые модули `regime_ensemble`, `meta_label`, `calibration` (~1387 LOC + тесты). Они были инициализированы, но не вызывались из pipeline. Активные ML-компоненты — те, что реально гейтят или модулируют решения на каждом тике/входе.

## Активные компоненты

| Компонент | Hot-path | Использование |
|-----------|----------|---------------|
| `EntropyFilter` | per-tick | блокирует тик при `is_noisy()` (gate в pipeline) |
| `LiquidationCascadeDetector` | per-tick | блокирует вход при `is_cascade_likely()` |
| `CorrelationMonitor` | per-tick | модулирует `combined_risk_multiplier` в `MlSignalSnapshot::compute_aggregates()` |
| `MicrostructureFingerprinter` | per-entry | блокирует вход если `fp_edge < -0.1` |
| `BayesianAdapter` | per-entry (после ≥20 observations) | адаптирует `conviction_threshold` и `atr_stop_mult` |
| `ThompsonSampler` | per-entry | выбирает `EnterNow` / `Wait1..N` (multi-armed bandit) |

## Публичные интерфейсы

* `BayesianAdapter` — Bayesian update параметров стратегии.
* `ThompsonSampler` — multi-armed bandit для `EntryAction ∈ {EnterNow, Wait1..N}`.
* `EntropyFilter` — cross-entropy фильтрация шумных сигналов.
* `MicrostructureFingerprinter` — отпечаток рынка для записи + сравнения.
* `LiquidationCascadeDetector` — оценка каскадных ликвидаций.
* `CorrelationMonitor` — multi-asset correlation (BTC/ETH reference feeds).
* `MlSignalSnapshot` — агрегатное DTO для downstream consumer'ов.

## Что было удалено

* `regime_ensemble.{hpp,cpp}` (281 LOC) — не вызывался.
* `meta_label.{hpp,cpp}` (351 LOC) — не вызывался.
* `calibration.{hpp,cpp}` (276 LOC) — не вызывался; импортировался только из удалённого `regime_ensemble`.
* Соответствующие тесты в `tests/unit/ml/`.

`MlSignalSnapshot` сохранил структуру для downstream consumer'ов — поля, специфичные для удалённых компонентов, удалены либо игнорируются.

## Внутренние компоненты

* `bayesian_adapter.hpp/cpp`.
* `thompson_sampler.hpp/cpp`.
* `entropy_filter.hpp/cpp`.
* `microstructure_fingerprint.hpp/cpp`.
* `liquidation_cascade.hpp/cpp`.
* `correlation_monitor.hpp/cpp`.
* `ml_signal_types.hpp` — `MlSignalSnapshot`, `EntryAction`.

## Зависимости

* `features`, `regime`, `world_model`, `uncertainty`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
TradingPipeline на каждом тике:
  entropy_filter_.update(features) → is_noisy? → gate
  cascade_detector_.on_tick(features) → is_cascade_likely? → gate
  correlation_monitor_.on_tick(symbol, price, ref_btc, ref_eth) → multiplier
  fingerprinter_.compute(features) → snapshot field
  → MlSignalSnapshot (aggregate)

На входе (после decision approval):
  fingerprinter_.lookup_edge(intent) → fp_edge → gate (< -0.1)
  if observations ≥ 20: bayesian_adapter_.adapt(...) → modulate threshold/atr_stop
  thompson_sampler_.select_action(symbol) → EnterNow / Wait1..N
```

## Concurrency

Каждый компонент имеет собственное состояние, защищённое mutex или atomic.

## Текущие риски

* **D-ml-1 (RESOLVED).** Thompson sampling exploration deadlock (раньше `pulls = 0` → 4/5 arms давали Wait) — исправлено в session 4 (всегда EnterNow до первого feedback). См. memory `session4_fixes.md`.
* **D-ml-2 (MEDIUM).** Несколько ML-компонентов агрегируются в `MlSignalSnapshot` — при сбое одного `is_valid = false` влияет на весь snapshot.
* **D-ml-3 (MEDIUM).** `MicrostructureFingerprinter` — fingerprint cardinality зависит от bucketing; memory bound TBD.
* **D-ml-5 (LOW).** Reference prices для `CorrelationMonitor` обновляются раз в 30 сек через background thread; в gap'ах коррелация замирает.

## Рекомендации

1. Стандартный ML interface: `class IMlComponent { update(snapshot); query(...); reset(); }`.
2. Per-component health check: `is_ready()` / `is_drifting()`.
3. Метрики: thompson distribution, entropy filter accept rate, fingerprint cluster count.
4. Регрессия на исторических трейсах, comparing decisions с/без каждого активного ML-компонента.
