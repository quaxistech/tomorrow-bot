# `src/ml` — ML-компоненты

## Назначение

Набор статистических/ML моделей для адаптации стратегии: байесовские параметры, multi-armed bandit (Thompson), entropy filter, microstructure fingerprinting, liquidation cascade detection, multi-asset correlation, regime ensemble, calibration, meta-labeling.

## Границы ответственности

* Подавление шумных сигналов (entropy filter, fingerprint suppress).
* Адаптация параметров стратегии под наблюдаемое поведение (Bayesian).
* Выбор момента входа (Thompson sampling: EnterNow/Wait1/Wait2/...).
* Распознавание мульти-активных режимов (`CorrelationMonitor`).
* Контроль каскадной ликвидации.
* Калибровка вероятностей (`calibration`).
* Meta-labeling (advanced filter сигналов).

## Публичные интерфейсы (по головным заголовкам)

* `class BayesianAdapter` — обновление параметров стратегии через Bayesian update.
* `class ThompsonSampler` — multi-armed bandit для выбора `EntryAction ∈ {EnterNow, Wait1..N}`.
* `class EntropyFilter` — фильтрует сигналы по cross-entropy.
* `class MicrostructureFingerprinter` — отпечаток рынка (для записи и сравнения).
* `class LiquidationCascadeDetector` — оценка каскадных ликвидаций.
* `class CorrelationMonitor` — multi-asset correlation (BTC/ETH reference feeds).
* `class RegimeEnsemble` — ensemble классификатор поверх `RegimeEngine`.
* `Calibration` — вероятностная калибровка.
* `MetaLabel` — meta-labeling (Lopez de Prado).
* `MlSignalSnapshot` — DTO с агрегированным ML state.

## Внутренние компоненты

* `bayesian_adapter.hpp/cpp`.
* `thompson_sampler.hpp/cpp`.
* `entropy_filter.hpp/cpp`.
* `microstructure_fingerprint.hpp/cpp`.
* `liquidation_cascade.hpp/cpp`.
* `correlation_monitor.hpp/cpp`.
* `regime_ensemble.hpp/cpp`.
* `calibration.hpp/cpp`.
* `meta_label.hpp/cpp`.
* `ml_signal_types.hpp` — DTO + `EntryAction`.

## Зависимости

* `features`, `regime`, `world_model`, `uncertainty`.
* `clock`, `logging`, `metrics`.

## Потоки данных

```
TradingPipeline на каждом тике:
  bayesian_adapter_.update(features, last_outcome)
  fingerprint = fingerprinter_.compute(features)
  cascade_signal = cascade_detector_.detect(...)
  correlation_signal = correlation_monitor_.update(price, ref_prices_BTC_ETH)
  ml_snapshot = aggregate to MlSignalSnapshot
  ...
  decision → если conviction > threshold:
    entropy_filter_.allow(intent) → bool
    fingerprint_suppress?
    thompson_sampler_.select_action(intent.symbol) → {EnterNow, Wait1, ...}
```

## Race conditions

Каждый компонент имеет собственное состояние, защищённое mutex (или atomic).

## Ошибки проектирования

* **D-ml-1 (HIGH).** Thompson sampling: `pulls` инкрементируется только на закрытие сделки. До первой сделки `pulls = 0` → exploration deadlock. Исправлено в session 4: «всегда EnterNow до первого feedback».
* **D-ml-2 (MEDIUM).** Множество ML компонентов агрегируется в `MlSignalSnapshot` — если хотя бы один не готов, весь snapshot имеет `is_valid = false` (требует верификации).
* **D-ml-3 (MEDIUM).** `MicrostructureFingerprinter` — fingerprint может приобретать high cardinality (зависит от bucketing). Memory bound TBD.
* **D-ml-4 (LOW).** Калибровка вероятностей не имеет явного recipe для retraining (offline procedure).
* **D-ml-5 (LOW).** Reference prices для `CorrelationMonitor` обновляются раз в 30 сек через background thread; в gap'ах коррелация замирает.

## Контракты

### `ThompsonSampler::select_action(symbol) → EntryAction`

* **Pre.** Symbol зарегистрирован.
* **Post.** Возвращена одна из 5 действий. Если `total_pulls = 0` → `EnterNow` (см. D-ml-1).
* **Invariant.** За многими вызовами с одним symbol распределение действий стремится к Bayesian-optimal mix.

### `EntropyFilter::allow(intent, ...)` → `bool`

* **Pre.** Intent валидный.
* **Post.** True/False. False — сигнал шумный (high entropy).

## Производственные риски

* **R-ml-1.** Любая модель из ML-стека может «зависнуть» в неверном состоянии (стейл данные, плохой fingerprint). Mitigation: `OperationalGuard` + явный reset на reconcile mismatch.
* **R-ml-2.** ML-компоненты увеличивают coupling между модулями.

## Рекомендации

1. Standard ML interface: `class IMlComponent { update(snapshot); query(...); reset(); }`.
2. Per-component health check: `is_ready()`/`is_drifting()`.
3. Метрики: thompson distribution, entropy filter accept rate, fingerprint cluster count.
4. Test: regression на исторических трейсах, comparing decisions с/без каждого ML компонента.
5. Документировать тренинг пайплайн (если есть offline training).
