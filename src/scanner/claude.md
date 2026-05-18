# src/scanner — Scalping-Optimized Market Scanner (EDGE-31)

## Назначение

Отбор USDT-M futures пар, **executable для scalping** ($5-$50 orders) с минимальным slippage, устойчивой microstructure и понятной predictability.

**НЕ оптимизирует "активность вообще".** Hard gates отсеивают unexecutable pairs ДО ranking. Soft composite ranks scalpability среди survivors. Pairs классифицируются по `regime_tag` (Momentum / MeanReversion / Breakout / Avoid) для strategy matching.

## Pipeline

```
tickers (553)
  → PreFilter (status, blacklist, min_volume, max_spread × 3, min_oi, change_24h_range)
  → top 100 candidates (max_candidates_detailed=100)
  → ParallelFetch orderbook + candles (REST)
  → FeatureCalculator → SymbolFeatures
        ├── LiquidityFeatures (depth_near_mid + depth_0.5pct + depth_1pct)
        ├── SpreadFeatures (spread_bps)
        ├── VolatilityFeatures (atr, realized_vol_1m/5m, vol_quality_score)
        ├── OrderBookFeatures (imbalance, walls, slippage_at_10/50usdt, resiliency)
        ├── TrendQualityFeatures (trend, pullback, momentum, wick_ratio, body_ratio, trade_count_1m)
        └── AnomalyFeatures (vol spike, OI divergence, noise)
  → TrapAggregator (spoofing, stop-hunt, momentum trap, fragility)
  → PairFilter (hard gates — REJECT if any fail)
        Legacy: liquidity / spread / oi / depth / trap_risk / vol / 24h_change
        Scalping (если scalping.enabled):
          spread_bps ≤ hg_max_spread_bps
          depth_1pct ≥ hg_min_book_depth_1pct_usdt
          slippage_$10 ≤ hg_max_slippage_at_10usdt_bps
          resiliency ≥ hg_min_resiliency
          |funding| ≤ hg_max_funding_abs
          vol_1m в [hg_min_realized_vol_1m_pct, hg_max_realized_vol_1m_pct]
          wick_ratio ≤ hg_max_wick_ratio
  → PairRanker → SymbolScore + scalpability_score + regime_tag
  → BiasDetector (Long/Short bias confidence)
  → diversify_basket (correlation + sector cap)
  → top_n
```

## Hard Gates (EDGE-31 scalping cliffs)

Любая failed → REJECT (no soft compensation).

| Gate | Default | Rationale |
|---|---|---|
| `hg_max_spread_bps` | **8** | Round-trip taker 12 bps. Spread > 8 = ~50% эффективности |
| `hg_min_book_depth_1pct_usdt` | **50,000** | Depth > 5× target notional ($10 × 5) для resiliency |
| `hg_max_slippage_at_10usdt_bps` | **3** | < 25% от fee burden, не съедает margin ROI |
| `hg_min_trade_count_1m` | **50** | Continuous flow, не dead |
| `hg_min_resiliency` | **0.4** | Book recovers быстро после take |
| `hg_max_funding_abs` | **0.0005** | Не платим extreme carry |
| `hg_min_realized_vol_1m_pct` | **0.10** | Live instrument |
| `hg_max_realized_vol_1m_pct` | **1.5** | Не chaos |
| `hg_max_wick_ratio` | **0.6** | Anti-stop-hunt |

## Composite Scalpability Score (soft, sum_weights ≈ 1.0)

```
scalpability_score =
    w_spread × spread_score          # 0.20
  + w_depth × depth_score             # 0.15
  + w_resiliency × resiliency_score   # 0.12
  + w_vol_quality × vol_quality       # 0.15 — peak @ 0.30% vol_1m
  + w_trade_flow × trade_flow         # 0.12
  + w_micro_structure × micro         # 0.10 — body × (1-wick)
  + w_execution_quality × exec_q      # 0.10
  + w_regime_match × regime_match     # 0.06
  - p_trap_risk × trap_aggregate      # 0.30
  - p_funding_drift × funding_pen     # 0.15
  - p_btc_correlation × btc_excess    # 0.10 (placeholder)
```

## Regime Tag Classifier (inline в PairRanker)

```cpp
if (vol_quality > 0.5 && momentum_persistence > 0.6 && body_to_range > 0.5):
    tag = Momentum         → MomentumContinuation strategy
else if (vol_quality > 0.4 && micro_trend_strength < 0.3):
    tag = MeanReversion    → BB-edge fade
else if (has_impulse && body_to_range > 0.6):
    tag = Breakout         → squeeze breakout
else:
    tag = Avoid            → no scalp setup
```

## Новые метрики (EDGE-31)

Все в SymbolFeatures, вычисляются в `FeatureCalculator::compute()`:

| Метрика | Где | Формула |
|---|---|---|
| `book_depth_0_5pct_usdt` | liquidity | sum bid+ask notional within ±0.5% mid |
| `book_depth_1pct_usdt` | liquidity | sum bid+ask notional within ±1.0% mid |
| `slippage_at_10usdt_bps` | orderbook | walk asks $10, (avg_fill-mid)/mid × 10000 |
| `slippage_at_50usdt_bps` | orderbook | same for $50 |
| `resiliency_score` | orderbook | (depth_1pct - $50) / depth_1pct, 0-1 |
| `realized_vol_1m_pct` | volatility | realized_vol_pct / sqrt(5) |
| `realized_vol_5m_pct` | volatility | == realized_vol_pct |
| `vol_quality_score` | volatility | 1 - |vol_1m - sweet_spot| / sweet_spot |
| `wick_ratio` | trend_quality | avg (upper_wick + lower_wick) / range |
| `body_to_range_ratio` | trend_quality | avg \|close-open\| / (high-low) |
| `trade_count_1m` | trend_quality | (candle_volume × mid / $100) / interval_min |

## Anti-patterns (auto-reject через hard gates)

1. **Volume без trade count**: high vol_24h, trade_count_1m < 50 → wash trading
2. **Tight spread без depth**: spread < 5 bps, depth_1pct < $20k → paper book
3. **Vol spike one-candle**: anomaly.volume_spike=true → no flow continuation
4. **Price/OI divergence**: anomaly.oi_price_divergence=true → no real demand
5. **Extreme funding**: |funding| > 0.0005 → overcrowded position
6. **Stop-hunt frequency**: wick_ratio > 0.6 → spikes выбивают stops
7. **Compressed без catalyst**: vol_1m < 0.10 → dead market

## Конфигурация (scanner_config.hpp::ScalpingProfile)

```cpp
struct ScalpingProfile {
    bool enabled{true};
    // Hard gates
    double hg_max_spread_bps{8.0};
    double hg_min_book_depth_1pct_usdt{50'000};
    double hg_max_slippage_at_10usdt_bps{3.0};
    double hg_min_trade_count_1m{50};
    double hg_min_resiliency{0.4};
    double hg_max_funding_abs{0.0005};
    double hg_min_realized_vol_1m_pct{0.10};
    double hg_max_realized_vol_1m_pct{1.5};
    double hg_max_wick_ratio{0.60};
    // Soft weights
    double w_spread{0.20}, w_depth{0.15}, w_resiliency{0.12};
    double w_vol_quality{0.15}, w_trade_flow{0.12};
    double w_micro_structure{0.10}, w_execution_quality{0.10};
    double w_regime_match{0.06};
    // Penalties
    double p_trap_risk{0.30}, p_funding_drift{0.15}, p_btc_correlation{0.10};
    double vol_sweet_spot_pct{0.30};
};
```

## Backward compatibility

`scalping.enabled = false` → legacy ranking. Существующие интерфейсы `ScannerResult` и `ScannerEngine::scan()` не изменены.

## run90 calibration (2026-05-17): thin orderbook filter

После анализа FFUSDT/OPGUSDT (висели часами на тонкой ликвидности с OB score 0.04-0.07):

```cpp
// scanner_config.hpp
double min_orderbook_depth_usdt{30'000.0};      // 20k → 30k (run90)
double micro_min_orderbook_depth_usdt{5'000.0}; // 1500 → 5000 (run90)
```

В `production.yaml`:
- `min_volume_usdt: 2000000` (было 800k) — для реальной активности
- micro orderbook depth применяется при `capital < micro_account_capital_threshold_usdt=$100`

## Future work (Phase 3+)

- `MarketRegimeDetector` (BTC/ETH 5m → global tag)
- `ExecutionQualityScorer` отдельный module (maker fill probability)
- `ScalpingDiversifier` (microstructure cluster avoidance)
- Trades data fetch для real `trade_count_1m`
- Funding stability history (variance of last 8 rates)

## Файлы

- `scanner_config.hpp` — структуры + ScalpingProfile
- `scanner_types.hpp` — SymbolFeatures + RegimeTag enum
- `feature_calculator.cpp` — compute всех 21+ метрик
- `pair_filter.cpp` — hard gates (legacy + scalping)
- `pair_ranker.cpp` — composite scalpability_score + regime_tag
- `scanner_engine.cpp` — pipeline (без изменений в interface)
- `trap_detectors.cpp` — existing traps (sustained)
