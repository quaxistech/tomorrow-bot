# Advanced Indicators (run94 expansion)

**Файлы:** `advanced_indicators.{hpp,cpp}`
**Введено:** run94 (2026-05-17) — professional toolkit для signal precision

## 10 индикаторов

### 1. Anchored VWAP (`AnchoredVwap` class, stateful streaming)
Carter (2012). Daily anchor (UTC reset 00:00). +1σ/+2σ bands из volume-weighted variance.
- `on_trade(price, volume, ts_ns)` — обновление на каждый trade.
- `snapshot(current_price) → AnchoredVwapResult`:
  - `vwap`, `upper/lower_1sigma`, `upper/lower_2sigma`, `price_vs_vwap_bps`.
- Auto-reset на новый UTC day. Можно явный `reset(anchor_ts_ns)`.

### 2. CVD Tracker (`CvdTracker`, stateful streaming)
Cumulative Volume Delta = Σ(taker_buy_volume - taker_sell_volume).
- `on_trade(price, volume, taker_buy, ts_ns)`.
- `snapshot() → CvdResult`:
  - `cvd` (cumulative), `cvd_change_recent` (Σ за recent_window), `cvd_normalized` (∈ [-1,+1]).
  - `bullish_divergence`: price LL but CVD HL (buyers accumulating into drop).
  - `bearish_divergence`: price HH but CVD falling (distribution into rally).
- Divergence detect: split window на 2 половины, сравнить high/low + cvd delta.

### 3. OI Tracker (`OiTracker`, stateful streaming)
Wyckoff 4-quadrant model:
- `on_oi_update(oi_usdt, current_price, ts_ns)`.
- `snapshot() → OiResult`:
  - `oi_change_recent_pct` (% from window start to now).
  - `trend_quadrant`: 1=OI↑+Px↑ (new longs, healthy uptrend),
    2=OI↑+Px↓ (new shorts, healthy down),
    3=OI↓+Px↑ (shorts covering, weak rally),
    4=OI↓+Px↓ (longs liq, weak drop).

### 4. Liquidity Sweep (`detect_liquidity_sweep`, pure function)
Smart-money concept: wick beyond local high/low + revert back signals stop-hunt.
- Input: `vector<double> highs/lows/closes` (последние ≥ lookback+1 свечей).
- Output: `LiquiditySweepResult` с `sweep_high/low_detected`, `recovery_pct`.
- Параметры: `wick_ratio_threshold` (0.5 по умолчанию), `recovery_threshold` (0.6).
- Use: НЕ открывать в направлении wick (fake breakout), fade signal.

### 5. Queue Position Estimator (`estimate_queue_position`, pure function)
Cont-Larrard (2013). Estimate position в FIFO очереди на best level.
- Input: our_order_usdt, best_level_size_usdt, queue_depletion_rate.
- Output: `estimated_position` (0=front, 1=back), `p_fill_30s`.
- Used by execution_alpha для решения post-only vs aggressive.

### 6. Spoof Detector (`detect_spoofing`, pure function)
Cartea-Jaimungal (2015). Признаки: wall > 30% депs, активный cancel burst, асимметричный refill.
- Input: bid/ask depths, top sizes, cancel_burst_intensity, refill_asymmetry.
- Output: `spoof_bid/ask_detected`, `spoof_intensity` (0-1).
- При intensity > 0.7 — снижаем conviction (degrade signal).

### 7. Liquidation Heatmap proxy (`estimate_liquidation_clusters`, pure function)
Реальный heatmap требует liquidation feed. Этот proxy estimate cluster levels из:
- OI delta velocity (новые позиции = новые liq levels).
- Funding extremes (skewed positioning).
- Price momentum (cascade probability).
- Output: `upside/downside_liq_cluster_pct`, `cascade_risk_score`, `dominant_side`.

### 8. Funding Bias (`evaluate_funding_bias`, pure function)
Funding rate как proxy positioning crowding.
- Positive extreme (> 0.001 = 10 bps/8h): longs crowded → mean-revert short edge.
- Negative extreme: shorts crowded → long edge.
- Output: `crowding_side`, `crowding_intensity`, `recommended_bias` (mean-revert).

### 9. Adaptive Thresholds (`compute_adaptive_thresholds`, pure function)
Vol-normalized RSI/momentum/BB thresholds.
- High vol → wider RSI (нужны более extreme values для signal): vol×2 → RSI OB=75, OS=25.
- Vol-scaled momentum thresholds.
- BB breakout pct_b adapts.

### 10. Bayesian Signal Combiner (`combine_signals_bayesian`, pure function)
Naive Bayes fusion из множества indicator likelihoods.
- Input: `vector<SignalLikelihood>{lr_bullish, lr_bearish}`.
- Computation: `posterior_odds = prior_odds × Π LR_i`. LR capped [0.05, 20] для stability.
- Output: `p_bullish`, `p_bearish`, `confidence` (= 1 - 2 × min(p_bull,p_bear)), `dominant_direction`.
- Used in `strategy/setups/setup_lifecycle.cpp:detect_momentum` — 10 indicators → posterior probability, reject если < 0.55.

## Stateful trackers (FeatureEngine ownership)

Per-symbol мапы:
```cpp
unordered_map<string, AnchoredVwap> avwap_trackers_;
unordered_map<string, CvdTracker> cvd_trackers_;
unordered_map<string, OiTracker> oi_trackers_;
unordered_map<string, double> funding_rates_;
```

Обновляются:
- `on_trade(trade)` — AVWAP + CVD (taker_buy = trade.side == Buy).
- `update_open_interest(symbol, oi_usdt, ts_ns)` — OI tracker.
- `update_funding_rate(symbol, rate)` — funding cache.

Snapshot выдаётся в `FeatureSnapshot.technical` через compute_technical().

## Bayesian fusion в strategy

10 indicators дают likelihood ratios → posterior P(bullish/bearish). См.
`src/strategy/setups/setup_lifecycle.cpp` detect_momentum для конкретных LR weights.
Setup проходит только при posterior > 0.55 в направлении сигнала.
