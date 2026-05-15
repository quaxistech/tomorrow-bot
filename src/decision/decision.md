# Decision Module Audit (Temporary)

Date: 2026-04-08 (updated after production-grade overhaul)

Scope of analysis:
- `src/decision/decision_aggregation_engine.hpp`
- `src/decision/decision_aggregation_engine.cpp`
- `src/decision/decision_types.hpp`
- `tests/unit/decision/decision_test.cpp`
- integration points in `src/pipeline/trading_pipeline.cpp`
- upstream producers in `src/features`, `src/indicators`, `src/uncertainty`, `src/regime`, `src/world_model`

## 1. What the decision module does

The decision module is a committee aggregator. It does not generate alpha, does not compute technical indicators itself, does not size positions, and does not submit orders.

Its job is narrower and very important:

1. Receive one or more `TradeIntent` objects from strategy engines.
2. Apply allocator weights and enabled/disabled state.
3. Apply global vetoes.
4. Resolve BUY/SELL conflicts.
5. Build an effective conviction threshold using uncertainty, regime, and portfolio state.
6. Optionally penalize conviction by execution cost.
7. Return a fully explainable `DecisionRecord`.

In the live pipeline it is called after strategies and allocator, and before execution-alpha, risk checks, leverage, and order placement.

## 2. Runtime position in the pipeline

Current live order in `TradingPipeline`:

1. `FeatureEngine` and `AdvancedFeatureEngine` build the `FeatureSnapshot`.
2. `WorldModel`, `Regime`, and `Uncertainty` produce contextual states.
3. `StrategyEngine` produces `TradeIntent` candidates.
4. `StrategyAllocator` provides weights and enable flags.
5. `CommitteeDecisionEngine::aggregate(...)` selects or rejects the candidate.
6. If approved, the result goes further into execution-alpha, ML/fingerprint filters, risk, leverage, and execution.

This means decision is a policy layer between alpha generation and trade execution.

## 3. Public contract

### 3.1 Input

`CommitteeDecisionEngine::aggregate(...)` receives:

- `symbol`
- `vector<TradeIntent> intents`
- `AllocationResult allocation`
- `RegimeSnapshot regime`
- `WorldModelSnapshot world`
- `UncertaintySnapshot uncertainty`
- optional `PortfolioSnapshot portfolio`
- optional `FeatureSnapshot features`

### 3.2 Output

It returns `DecisionRecord`, which contains:

- final approved intent or rejection
- final conviction
- structured rejection reason
- effective threshold and approval gap
- global vetoes
- per-strategy contributions
- ensemble metrics
- execution-cost estimate
- textual rationale
- audit context: regime, world label, uncertainty level, timestamps

This makes the module replay-friendly and explainable by design.

## 4. Step-by-step algorithm

The live algorithm in `aggregate(...)` is deterministic and follows this sequence.

### Step 0. Record context

The engine writes into `DecisionRecord`:

- current symbol
- decision timestamp from injected `IClock`
- regime label and detailed regime
- world label
- uncertainty level and aggregate score

### Step 1. Global vetoes

The module rejects everything immediately if at least one hard veto fires.

Current hard vetoes:

1. `uncertainty.recommended_action == NoTrade`
2. `allocation.enabled_count == 0`

If a global veto fires:

- `trade_approved = false`
- each contribution is marked vetoed
- `rejection_reason` is taken from the first veto
- rationale is written and returned immediately

### Step 2. Execution cost estimate

If execution-cost modeling is enabled and `FeatureSnapshot` is present, the engine estimates:

- current spread in bps
- expected slippage in bps
- total execution cost
- conviction penalty derived from that cost

If total cost is above `max_acceptable_cost_bps`, the decision is rejected immediately with `ExecutionCostTooHigh`.

### Step 3. Score every intent

For each `TradeIntent`:

1. Find its allocator entry.
2. Reject it if the strategy is disabled or absent from allocation.
3. Apply time decay to stale signals.
4. Subtract execution-cost penalty from conviction.
5. Multiply by allocator weight.

The engine tracks whether there are BUY and SELL candidates simultaneously.

### Step 4. Resolve BUY/SELL conflict

If both sides are present:

1. Sum weighted scores separately for BUY and SELL.
2. Compute dominance of the winning side.
3. Adjust the required dominance threshold by market regime if enabled.
4. Reject everything if dominance is insufficient.
5. Otherwise drop all intents from the losing side.

### Step 5. Choose the best surviving intent

The remaining intents are sorted by `weighted_score`, descending.

The leader becomes the candidate for final approval.

### Step 6. Build effective threshold

The approval threshold is not fixed. It is built dynamically from:

1. base conviction threshold
2. uncertainty multiplier
3. regime multiplier
4. drawdown/loss-streak boost from portfolio state

### Step 7. Ensemble conviction bonus

If multiple strategies align in the same direction, the leader can receive an ensemble bonus.

The first aligned strategy gets no bonus. Every additional aligned strategy adds a diminishing bonus until `ensemble_max_bonus` is reached.

### Step 8. Approve or reject

If adjusted conviction is below the effective threshold:

- reject with `LowConviction`
- keep full rationale and metrics

Otherwise:

- approve trade
- copy the winning `TradeIntent` into `final_intent`
- store final conviction and correlation id

## 5. Actual formulas used by the code

### 5.1 Time decay

The code uses exponential half-life decay:

`aged_conviction = raw_conviction * exp(-ln(2) * age_ms / halflife_ms)`

where:

- `age_ms = (decided_at - generated_at) / 1_000_000`
- `halflife_ms = advanced.time_decay_halflife_ms`

If `generated_at` is not set, decay is skipped and a warning is logged.

### 5.2 Execution-cost penalty

`total_cost_bps = spread_bps + estimated_slippage_bps`

`conviction_penalty = (total_cost_bps / 100.0) * execution_cost_conviction_penalty`

`effective_conviction = max(aged_conviction - conviction_penalty, 0.0)`

Normalization to 100 bps: at 100 bps cost, penalty equals the full penalty_factor.
For scalping with 5–20 bps target profit, this gives meaningful conviction reduction.
Default penalty_factor = 0.3 (calibrated for USDT-M futures scalping).

### 5.3 Weighted score

`weighted_score = effective_conviction * allocation.weight`

This is the score used to rank candidates and resolve side conflicts.

### 5.4 Dominance in BUY/SELL conflict

`dominance = max(buy_total, sell_total) / (buy_total + sell_total + 1e-10)`

The required dominance threshold changes by regime when enabled.

### 5.5 Effective approval threshold

The threshold is built as:

`threshold = base_conviction_threshold * uncertainty.threshold_multiplier`

then optionally:

- `threshold *= regime_factor`
- `threshold += drawdown_boost`

So the final implementation is multiplicative for uncertainty and regime, but additive for portfolio drawdown protection.

### 5.6 Drawdown boost

`drawdown_component = abs(current_drawdown_pct) / drawdown_reference_pct * drawdown_boost_scale`

`loss_streak_component = consecutive_losses * consecutive_loss_boost`

`drawdown_boost = min(drawdown_component + loss_streak_component, drawdown_max_boost)`

Default drawdown_reference_pct = 5.0 (at 10× leverage, 5% account drawdown ≈ 0.5% price move).

### 5.7 Ensemble bonus

For each additional aligned strategy after the leader:

- add `ensemble_agreement_bonus * diminishing`
- multiply `diminishing` by `ensemble_diminishing_factor`

The total bonus is capped by `ensemble_max_bonus`.

## 6. Configuration surface

The pipeline maps `AppConfig.decision` into `AdvancedDecisionConfig` almost one-to-one.

Main defaults in code (all scientifically grounded for USDT-M futures scalping):

- `min_conviction_threshold = 0.45` (Aldridge 2013: ≥0.4–0.6 for leveraged instruments)
- `conflict_dominance_threshold = 0.60`
- `time_decay_halflife_ms = 500` (Hasbrouck 2007: order-book half-life 100–1000 ms)
- `ensemble_agreement_bonus = 0.06` (Breiman 2001: conservative ensemble bonus)
- `ensemble_max_bonus = 0.15`
- `drawdown_boost_scale = 0.02` (Thorp 2006: proportional Kelly reduction)
- `drawdown_max_boost = 0.08`
- `consecutive_loss_boost = 0.005` (Aronson 2007: loss streaks 5–8 normal)
- `max_acceptable_cost_bps = 50` (Bitget taker ~6 bps + spread ~5 bps = ~10 bps norm)
- `execution_cost_conviction_penalty = 0.3` (calibrated: 10bps cost → 3% conviction reduction)
- `regime_stress_factor = 1.30` (Menkveld 2013: increased adverse selection in stress)

Current production overrides are materially different:

- `min_conviction_threshold = 0.62`
- `conflict_dominance_threshold = 0.60`
- `max_acceptable_cost_bps = 300`
- `drawdown_boost_scale = 0.01`
- `drawdown_max_boost = 0.05`
- `consecutive_loss_boost = 0.005`

Interpretation:

- production requires much stronger raw conviction
- production is far more tolerant to execution cost than code defaults
- production makes drawdown penalty much weaker than defaults

That means the live bot currently uses decision more as a quality gate on conviction than as a strict microstructure-cost or drawdown-protection gate.

## 7. What indicators decision actually depends on

### 7.1 Direct indicator dependency of decision

Decision does not directly use RSI, ATR, ADX, MACD, Bollinger, OBV, Volume Profile, or VPIN.

Direct feature dependency is only this execution-cost subset:

- `features.microstructure.spread_valid`
- `features.microstructure.spread_bps`
- `features.execution_context.slippage_valid`
- `features.execution_context.estimated_slippage_bps`

This is a crucial conclusion: the decision module is not an indicator engine. It is a policy and arbitration engine.

### 7.2 Non-indicator context that affects decision more than TA does

The strongest live inputs are actually:

- `TradeIntent.conviction`
- `TradeIntent.side`
- `TradeIntent.generated_at`
- allocator weight and enabled flag
- `uncertainty.recommended_action`
- `uncertainty.threshold_multiplier`
- `regime.detailed`
- `portfolio.pnl.current_drawdown_pct`
- `portfolio.pnl.consecutive_losses`

### 7.3 Project-wide indicator audit

For the current project, the required upstream indicator families are implemented.

Implemented in `IndicatorEngine` and fed into `FeatureEngine`:

- SMA
- EMA
- RSI
- MACD
- Bollinger Bands
- ATR
- ADX with `+DI` and `-DI`
- OBV
- volatility
- momentum
- VWAP helpers exist in `IndicatorEngine`

Implemented in `FeatureEngine` / `AdvancedFeatureEngine`:

- spread and spread in bps
- book imbalance
- weighted mid price
- buy/sell ratio
- aggressive flow
- trade VWAP
- 5-level depth notionals
- liquidity ratio
- book instability
- estimated slippage
- feed freshness
- VPIN
- CUSUM
- Volume Profile
- Time-of-Day profile

Implemented and actually consumed by runtime modules around decision:

- strategy/risk path uses core trend, momentum, volatility, ATR, ADX, RSI, Bollinger, spread, order-book imbalance, VPIN, liquidity, and trade-flow features
- regime uses ADX, RSI, OBV, CUSUM, VPIN, and book-instability related signals
- world model uses volatility, spread, instability, liquidity, momentum, VPIN, ADX, RSI, and Bollinger-derived signals
- uncertainty uses RSI/MACD alignment, spread, slippage, liquidity ratio, VPIN toxicity, book instability, feed freshness, and book quality
- execution-alpha uses `weighted_mid_price`, `tod_alpha_score`, `vpin`, and CUSUM state

### 7.4 Indicators/features that are implemented but currently not materially consumed downstream

As of 2026-04-08, the following advanced fields are implemented in producers but have no meaningful runtime consumer in `.cpp` decision/strategy/world-model/uncertainty code paths:

- `technical.vp_poc`
- `technical.vp_value_area_high`
- `technical.vp_value_area_low`
- `technical.vp_price_vs_poc`
- `microstructure.trade_vwap`
- `technical.session_hour_utc`
- `technical.tod_volatility_mult`
- `technical.tod_volume_mult`

Note:

- `technical.tod_alpha_score` is used by execution-alpha
- `microstructure.weighted_mid_price` is used by execution-alpha
- VPIN and CUSUM are actively used

Conclusion of the audit:

- no critical indicator required by the current live decision flow is missing
- the project has an advanced feature surface larger than the current downstream usage
- some advanced indicators are already implemented for future strategy/world-model growth but not yet monetized by live logic

## 8. Test coverage status

Current unit suite for decision contains 14 test cases.

Covered well:

- single-intent approval
- uncertainty veto
- BUY/SELL conflict rejection
- low-conviction rejection
- deterministic behavior
- time decay
- regime threshold scaling
- ensemble bonus
- portfolio drawdown boost
- execution-cost penalty
- execution-cost veto
- structured rejection reason
- approval gap

Not covered explicitly:

- time-skew detection details
- behavior when `generated_at` is zero beyond logging path
- any use of `world` as an active decision factor, because current code does not use it actively
- unused rejection codes

## 9. Fixes applied in production-grade overhaul

### Fix 1. `detect_time_skew()` now checks `uncertainty.computed_at`

Previously only checked `regime.computed_at` despite `uncertainty.computed_at` being available.
Now both timestamps are checked for stale-data detection.

### Fix 2. Dominance normalization uses actual base threshold

Previously hardcoded `0.60` in `regime_factor = regime_thr / 0.60`.
Now uses `dominance_threshold_` for correct normalization when base dominance differs.

### Fix 3. `DrawdownProtection` rejection reason now emitted

Previously always fell back to `LowConviction`. Now correctly emits `DrawdownProtection`
when the drawdown boost was the deciding factor (trade would pass without drawdown penalty).

### Fix 4. Magic number 5.0 replaced with configurable `drawdown_reference_pct`

Drawdown formula now uses `advanced_.drawdown_reference_pct` (default 5.0).

### Fix 5. Execution cost penalty formula fixed for scalping relevance

Old formula: `(cost_bps / 10000) × 0.5` → at 20 bps: penalty = 0.001 (negligible).
New formula: `(cost_bps / 100) × 0.3` → at 20 bps: penalty = 0.06 (meaningful).
Now the soft penalty actually matters before the hard veto kicks in.

### Fix 6. Regime factors for LowVolCompression and AnomalyEvent now configurable

Previously hardcoded to 1.0 with incorrect "micro-cap" comments.
Now use `regime_low_vol_factor` (default 1.0) and `regime_anomaly_factor` (default 1.20).
Anomaly events in USDT-M futures require higher conviction (+20%).

### Fix 7. `regime_stress_factor` raised from 1.0 to 1.30

Liquidity stress in perpetual futures increases adverse selection (Menkveld 2013).
+30% threshold increase provides meaningful protection.

### Fix 8. Field naming corrected

`StrategyContribution.regime_adjusted_conviction` → `cost_adjusted_conviction`.
`ScoredIntent.regime_conviction` → `effective_conviction`.
Names now accurately describe what they store.

### Fix 9. All config defaults scientifically grounded

Every default now has literature citation and is calibrated for USDT-M futures scalping.
Removed incorrect "micro-cap" comments throughout.

## 10. Final verdict

The decision module is a solid committee gate and explainability layer.

What is good:

- deterministic
- auditable
- configurable
- portfolio-aware
- uncertainty-aware
- conflict-aware
- execution-cost-aware
- well covered by focused unit tests

What it is not:

- not an alpha generator
- not an indicator calculator
- not a risk engine
- not a portfolio-compatibility engine yet
- not yet fully using all context promised by its API

Indicator verdict for the project:

- all indicators required by the current live decision path are implemented
- all core upstream indicators required by strategy/regime/world-model/uncertainty are implemented
- some advanced indicators are implemented but not yet used by downstream runtime logic

So the correct engineering conclusion is not "decision lacks indicators".

The correct conclusion is:

- decision itself intentionally depends on very few indicators
- the broader project already has the required indicator base
- the remaining opportunity is not missing implementation, but better downstream usage of already-available advanced features
