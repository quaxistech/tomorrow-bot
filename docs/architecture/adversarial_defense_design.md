# Adversarial Defense v4 — Институциональная защита от рыночных угроз

## Обзор

Модуль `AdversarialMarketDefense` — production-grade система обнаружения и
реагирования на враждебные рыночные условия. v4 реализует 14 детекторов угроз
и 7 продвинутых технологий анализа.

**Философия**: fail-closed (неизвестное = опасное), exit-always (продажа никогда
не блокируется), severity-based (градуированная реакция, не бинарная).

---

## 1. Типы угроз (14 детекторов)

### Базовые детекторы (v1)

| # | Тип | Описание | Действие |
|---|-----|----------|----------|
| 1 | `InvalidMarketState` | Невалидные входные данные | VetoTrade |
| 2 | `StaleMarketData` | Устаревшие данные (> max_age_ns) | VetoTrade |
| 3 | `SpreadExplosion` | Спред > порога | severity-based |
| 4 | `SpreadVelocitySpike` | Скорость расширения спреда > порога | severity-based |
| 5 | `LiquidityVacuum` | Глубина < минимума | severity-based |
| 6 | `UnstableOrderBook` | book_instability > порога | severity-based |
| 7 | `ToxicFlow` | Токсичный поток ордеров (ratio/flow/VPIN) | severity-based |
| 8 | `BadBreakoutTrap` | Комбинация: высокий спред + дисбаланс + нестабильность | severity-based |
| 9 | `PostShockCooldown` | Активный cooldown после шока | Cooldown |

### Продвинутые детекторы (v3)

| # | Тип | Описание | Действие |
|---|-----|----------|----------|
| 10 | `DepthAsymmetry` | Асимметрия bid/ask глубины (манипуляция spoofing) | severity-based |
| 11 | `AnomalousBaseline` | Z-score аномалия от адаптивного baseline | severity ≤ 0.65 |
| 12 | `ThreatEscalation` | Устойчивая серия угроз (N+ тиков подряд) | severity-based |

### Институциональные детекторы (v4)

| # | Тип | Описание | Действие |
|---|-----|----------|----------|
| 13 | `CorrelationBreakdown` | Резкий распад корреляции spread↔depth↔flow | severity ≤ 0.7 |
| 14 | `TimeframeDivergence` | Расхождение fast/slow baselines (z-score) | severity ≤ 0.6 |

---

## 2. Продвинутые технологии (v4)

### 2.1 Percentile-Based Scoring

Вместо линейных порогов используются эмпирические распределения.
Severity = позиция текущего значения в rolling окне (метод Hazen).

- Окно: 500 тиков (настраиваемо)
- Порог перцентиля: 95-й (настраиваемо)
- Применяется к: spread, depth, buy_sell_ratio
- Depth инвертирован (низкий = опасный)

### 2.2 Rolling Correlation Matrix

EMA-based корреляция Пирсона между тремя сигналами: spread, depth, flow.
Резкий распад корреляции — сильнейший ранний сигнал приближающегося шока.

- 3 пары: spread↔depth, spread↔flow, depth↔flow
- Детекция: |Δcorrelation| > порога (по умолчанию 0.4)
- Минимум 50 семплов для стабильности

### 2.3 Time-Weighted EMA

Адаптивный alpha зависит от интервала между тиками:

```
α_eff = 1 - exp(-dt · ln2 / halflife)
```

Корректно обрабатывает неравномерную частоту: 100ms и 5s тики
взвешиваются пропорционально реальному времени.

### 2.4 Multi-Timeframe Analysis

Три параллельных baseline с разными halflife:

| Горизонт | Halflife | Назначение |
|----------|----------|------------|
| Fast | ~30с | Текущая микроструктура |
| Medium | ~5мин | Среднесрочный тренд |
| Slow | ~30мин | Базовый уровень |

Расхождение fast vs slow (z-score > порога) = сильный сигнал
структурного изменения рынка.

### 2.5 Hysteresis

Предотвращает chattering (мерцание) на границе safe/unsafe:

```
Вход в danger zone:  compound_severity > enter_threshold (0.5)
Выход из danger zone: compound_severity < exit_threshold (0.25)
```

В danger zone применяется штраф confidence (по умолчанию -0.15).

### 2.6 Event Sourcing / Audit Log

Ring buffer из DefenseEvent записей для post-trade анализа:

```
DefenseEvent {
    timestamp_ms, symbol, action, compound_severity,
    confidence_multiplier, threshold_multiplier,
    regime, threat_count, is_safe, hysteresis_active
}
```

Максимум 10,000 записей (настраиваемо). Доступ: `get_audit_log()`.

### 2.7 Auto-Calibration Metrics

Накопление статистики для тонкой настройки порогов:

```
CalibrationMetrics {
    total_assessments, veto_count, cooldown_count,
    avg_compound_severity, max_compound_severity, veto_rate,
    hysteresis_activations/deactivations,
    per-threat type counts (×8 типов)
}
```

**Важно**: метрики READ-ONLY, пороги НЕ меняются автоматически.
Автоматическое изменение порогов в production слишком опасно.

---

## 3. Архитектура оценки

```
assess(MarketCondition) → DefenseAssessment
  1. Проверить cooldown (PostShockCooldown)
  2. Проверить InvalidMarketState / StaleMarketData
  3. Детекторы: SpreadExplosion, SpreadVelocity, LiquidityVacuum,
     UnstableBook, ToxicFlow, BadBreakout
  4. Продвинутые: DepthAsymmetry, AnomalousBaseline
  5. v4: CorrelationBreakdown, TimeframeDivergence
  6. ThreatEscalation (серия угроз)
  7. Compound severity (вероятностная модель)
  8. Cross-signal amplification
  9. Percentile severity overlay
  10. Классификация режима (Normal/Volatile/LowLiquidity/Toxic)
  11. Нелинейные кривые confidence/threshold
  12. Post-cooldown recovery
  13. Hysteresis (enter/exit danger zone)
  14. Threat memory (residual)
  15. Update state (baseline, MTF, percentile, correlation)
  16. Emit event + update calibration
```

### Приоритет действий

```
VetoTrade(5) > Cooldown(4) > AlertOperator(3) > RaiseThreshold(2) > ReduceConfidence(1) > NoAction(0)
```

### Compound Severity (вероятностная модель)

```
compound = (1-factor) × max_severity + factor × (1 - ∏(1 - severity_i))
```

### Нелинейные кривые

```
confidence_multiplier = 1.0 - severity² × max_reduction
threshold_multiplier  = 1.0 + severity^1.5 × max_expansion
```

---

## 4. Интеграция в Pipeline

### 5 точек интеграции:

```
┌─ TradingPipeline::on_feature_snapshot() ─────────────────┐
│                                                           │
│  1. assess() → DefenseAssessment                          │
│     Полная оценка каждый тик                              │
│                                                           │
│  2. is_safe → блокировка новых входов                     │
│     (exit/sell НИКОГДА не блокируется)                    │
│                                                           │
│  3. threshold_multiplier → ужесточение порога conviction  │
│     + regime-aware: Toxic +0.15, LowLiquidity +0.05      │
│                                                           │
│  4. confidence_multiplier → уменьшение размера позиции    │
│     + in_recovery → размер ≤ 50%                          │
│                                                           │
│  5. Логирование:                                          │
│     - Критичные действия (Veto/Cooldown) — каждый тик     │
│     - Прочие — каждые 100 тиков                           │
│     - Диагностика + калибровка — каждые 500 тиков         │
└───────────────────────────────────────────────────────────┘
```

### Поля DefenseAssessment используемые в pipeline:

| Поле | Назначение |
|------|------------|
| `is_safe` | Блокировка входа (2 точки) |
| `confidence_multiplier` | Множитель размера позиции |
| `threshold_multiplier` | Множитель порога conviction |
| `compound_severity` | Логирование, диагностика |
| `regime` | Regime-aware порог (+0.15/+0.05) |
| `hysteresis_active` | Логирование |
| `percentile_severity` | Логирование |
| `in_recovery` | Ограничение размера ≤ 50% |
| `threats` | Логирование типов + причин |
| `cooldown_remaining_ms` | Логирование |

---

## 5. Конфигурация

```yaml
adversarial_defense:
  enabled: true
  fail_closed_on_invalid_data: true
  auto_cooldown_on_veto: true
  auto_cooldown_severity: 0.85
  # --- Статические пороги ---
  spread_explosion_threshold_bps: 60
  spread_normal_bps: 15
  min_liquidity_depth: 500
  book_imbalance_threshold: 0.75
  book_instability_threshold: 0.65
  toxic_flow_ratio_threshold: 1.8
  aggressive_flow_threshold: 0.8
  vpin_toxic_threshold: 0.72
  cooldown_duration_ms: 30000
  post_shock_cooldown_ms: 60000
  max_market_data_age_ns: 2000000000
  max_confidence_reduction: 0.75
  max_threshold_expansion: 1.5
  # --- Compound & Recovery ---
  compound_threat_factor: 0.5
  cooldown_severity_scale: 1.5
  recovery_duration_ms: 10000
  recovery_confidence_floor: 0.6
  spread_velocity_threshold_bps_per_sec: 50.0
  # --- Adaptive Baseline ---
  baseline_alpha: 0.01
  baseline_warmup_ticks: 200
  z_score_spread_threshold: 3.0
  z_score_depth_threshold: 3.0
  z_score_ratio_threshold: 3.0
  baseline_stale_reset_ms: 300000
  # --- Threat Memory ---
  threat_memory_alpha: 0.15
  threat_memory_residual_factor: 0.3
  threat_escalation_ticks: 5
  threat_escalation_boost: 0.1
  # --- Depth Asymmetry ---
  depth_asymmetry_threshold: 0.3
  # --- Cross-Signal Amplification ---
  cross_signal_amplification: 0.3
  # --- v4: Percentile Scoring ---
  percentile_window_size: 500
  percentile_severity_threshold: 0.95
  # --- v4: Correlation Matrix ---
  correlation_alpha: 0.02
  correlation_breakdown_threshold: 0.4
  # --- v4: Multi-Timeframe Baselines ---
  baseline_halflife_fast_ms: 30000
  baseline_halflife_medium_ms: 300000
  baseline_halflife_slow_ms: 1800000
  timeframe_divergence_threshold: 2.5
  # --- v4: Hysteresis ---
  hysteresis_enter_severity: 0.5
  hysteresis_exit_severity: 0.25
  hysteresis_confidence_penalty: 0.15
  # --- v4: Event Sourcing ---
  audit_log_max_size: 10000
```

---

## 6. Диагностика и мониторинг

### API диагностики

```cpp
// Состояние для конкретного символа
DefenseDiagnostics diag = defense.get_diagnostics(symbol, now);
// → regime, baseline (EMA, z-scores), correlations, MTF, hysteresis, calibration

// Аудит-лог последних N решений
std::vector<DefenseEvent> log = defense.get_audit_log();

// Метрики калибровки
CalibrationMetrics cal = defense.get_calibration_metrics();
// → veto_rate, avg_severity, per-threat counts
```

### Периодическое логирование (каждые 500 тиков)

```json
{
  "category": "adversarial",
  "message": "Диагностика adversarial defense",
  "fields": {
    "symbol": "BTCUSDT",
    "regime": "Normal",
    "baseline_warm": "true",
    "baseline_samples": "1523",
    "spread_ema": "8.45",
    "hysteresis": "off",
    "corr_spread_depth": "-0.342",
    "fast_spread": "9.12",
    "slow_spread": "8.01",
    "total_assessments": "1523",
    "veto_rate": "0.003",
    "avg_severity": "0.021"
  }
}
```

---

## 7. Рекомендации по эксплуатации

- Мониторить `veto_rate` — нормальный: 0.1-3%, подозрительный: > 5%
- Проверять `avg_compound_severity` — нормальный: < 0.1, повышенный: > 0.3
- При высоком `hysteresis_activations` — рынок нестабилен, рассмотреть увеличение порогов
- Correlation breakdown — ранний предиктор шока, стоит алертить
- Cooldown НЕ отключать — это критический механизм безопасности
- Начинать с дефолтных порогов, тюнинг по CalibrationMetrics после 24+ часов данных

---

## 8. Тестирование

40 тестов, 212 assertions (Catch2 v3):

| Группа | Тесты | Описание |
|--------|-------|----------|
| Базовые | 26 | Все v1-v3 детекторы |
| Percentile Scoring | 2 | Extreme/normal values |
| Correlation Matrix | 1 | Breakdown detection |
| Time-Weighted EMA | 1 | Variable tick intervals |
| Multi-Timeframe | 1 | Divergence detection |
| Hysteresis | 3 | Activate/deactivate/penalty |
| Event Sourcing | 3 | Audit log, ring buffer, disable |
| Calibration | 2 | Accumulation, reset |
| Config Validation | 8 | All v4 params |
| to_string | 1 | New threat types |
| Diagnostics | 1 | v4 fields |
| Bridge | 1 | Config mapping |
