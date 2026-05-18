# Tomorrow Bot — Архитектурный аудит (architect-level)

**Версия проекта:** 0.5.0  
**Дата:** 2026-05-16 (edge-31 exit/TPSL refactor завершён)  
**Объём:** 38 модулей в `src/` (edge-31: +`pre_trade_gates`, +`protective_bracket_manager`, +`trade_journal`), C++23  
**Платформа:** Ubuntu 24.04, GCC 14, CMake 3.25+, Boost 1.83+, OpenSSL 3, libpqxx, yaml-cpp 0.8, Catch2 v3  
**Назначение:** автоматизированный скальпинг-бот для USDT-M фьючерсов на Bitget, hedge-mode, isolated margin

## edge-31 — Two-layer exit/TPSL architecture (2026-05-16)

Полная переработка защиты позиции. Все detail см. `docs/refactor_plan.md` (edge-31).

**Layer 1 (Exchange-native protection):**
* Entry order несёт `presetStopLossPrice` / `presetStopSurplusPrice` (`AttachedTpSl` копируется из `TradeIntent` в `OrderRecord`).
* `ProtectiveBracketManager` (`src/pipeline/protective_bracket_manager.*`) после fill verify'ит plan-ордера на бирже; если preset не зарегистрировался — ставит standalone fallback. На рестарте восстанавливает state через `recover_from_exchange()`.

**Layer 2 (Adaptive trailing):**
* `update_trailing_stop` в `trading_pipeline.cpp` вычисляет новый SL и через `bracket_manager_->update_sl()` (cancel+replace plan на бирже) пушит обновление. Anti-spam: min relative move 0.05% от цены.
* Локальные close-on-trailing-breach триггеры в orchestrator удалены.

**Pre-trade gates (Phase 2):**
* `FreshnessGate` — отклоняет stale signals (max_signal_age_ms=500, max_adverse_price_drift_bps=8.0, max_spread_widen_pct=50%, min_depth_remain_pct=50%).
* `NetRRGate` — отклоняет input с недостаточным net R:R после fees + slippage + funding (min_net_rr=0.5).
* Применяются ТОЛЬКО к entries (TradeSide::Open) сразу перед `execution_engine_->execute()`.

**Exit stack simplification:**
* `PositionExitOrchestrator::evaluate()` теперь содержит ТОЛЬКО 3 emergency-tier checks:
  1. `check_fixed_capital_stop` (safety net на отказ exchange SL)
  2. `check_toxic_flow` (VPIN toxic + meaningful loss)
  3. `check_stale_data_exit` (feed not fresh + adverse condition)
* Удалены 10 overlapping exits: `check_price_stop`, `check_trailing_stop`, `check_partial_tp`, `check_quick_profit`, `check_structural_failure`, `check_liquidity_deterioration`, `check_market_regime_exit`, `check_funding_carry_exit`, `compute_continuation_state`, `check_continuation_value_exit`, плюс inline EDGE-30 force_tp_high и fast_adverse tiers.

**TradeJournal (Phase 6):**
* Row-per-trade journal в `src/telemetry/trade_journal.*`.
* Поля: `signal_age_ms`, `entry/exit_price`, `mfe_bps`, `mae_bps`, `giveback_bps`, `gross_pnl`, `net_pnl`, `fees_paid`, `exit_layer` (HardCapital / ExchangeTP / ExchangeSL / TrailingSL / ToxicFlow / StaleData / SignalDriven / Manual), `hold_duration_ms`, `setup_type`, `conviction`.
* Lifecycle: `on_entry_filled` → `on_tick` (на каждом обновлении цены — MFE/MAE) → `on_exit_filled` (записывает row, эмитит structured log).

**Tests:** 659 / 659 passed, +36 новых тестов, ~10 obsolete тестов удалены.

> Документ задаёт инварианты, контракты, риск-профиль и план продолжающегося улучшения. Является источником истины для интеграции и регрессии. Любые изменения архитектуры должны верифицироваться против разделов 8 и 9.
>
> **Production-readiness статус (2026-05-08):**
> * Release build clean: `g++-14 -std=c++23 -O3 -DNDEBUG` (`build-check`).
> * Debug+ASAN+UBSAN build clean: `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined` (`build-asan`).
> * **698/698 тестов проходят в Release.**
> * **698/698 тестов проходят с ASAN+UBSAN — нулевые findings** (нет memory leaks, use-after-free, UB, integer overflow, bounds violations).
>
> **K8s integration:** `/livez` (liveness), `/readyz` (readiness), `/healthz` (alias) endpoints доступны через тот же `metrics_server`. Aggregated readiness учитывает: subsystems_started, ≥1 connected pipeline, kill_switch, degraded mode, shutdown_requested.
>
> **Связанные документы:**
> * [`docs/refactor_plan.md`](docs/refactor_plan.md) — статус всех дефектов с пометкой `IMPLEMENTED`.
> * `MEMORY.md` (auto-memory) — глобальные правила.
>
> **Live trading calibration (2026-05-14 / EDGE-15):**
> * 15 EDGE-fixes применены по data-driven analysis (см. `tools/quant/trade_analyzer.py`).
> * **EDGE-1**: momentum_min_buy/max_sell сбалансированы по знаку direction.
> * **EDGE-2**: PullbackInMicrotrend RSI guards (≤45 BUY, ≥55 SELL) — disabled.
> * **EDGE-3..8**: quality regime → consensus discipline (R:R, conf, spread).
> * **EDGE-9..11**: scanner weights bias на volatility+trend, vol threshold relax.
> * **EDGE-12**: funding-carry exit skip если hold<4h (settles на 8h boundary).
> * **EDGE-13**: SELL отключён по data evidence (n=16 PF=0.32).
> * **EDGE-14**: momentum thresholds 0.08%→0.25% (too strict, reverted).
> * **EDGE-15 (active mode)**: momentum 0.12%, conf 0.55, trap 0.85, depth 1.5k (балансируем idle vs noise).
> * Pullback/Retest/Rejection setups disabled — оставлен только MomentumContinuation.
> * **Data-driven blacklist** (14 symbols): BTC, ETH, XAG, XAU, BIO, BILL, LAB, DYM, ONDO, FIL, ASTER, LAYER, INJ, UB, INTC.
> * Доказанные положительные buckets: SAHARAUSDT (PF 11.68), Hold >30min (PF 3.27), edge_decay exit (PF 2.03).
>
> **Process hygiene:** перед запуском новой версии — kill всех живых `tomorrow-bot` процессов через `pkill -f "build-check/src/app/tomorrow-bot --config"`. Проверить отсутствие orphan через `pgrep`. PID файлы `logs/run*.pid` не cleanup автоматически — старые файлы могут указывать на освобождённые PIDs.
>
> **EDGE-16 (CRITICAL — run31 SAGA liquidation -$4.11):**
> На каждой scan rotation (10 min) создаётся новый `DualLegManager`, который теряет
> знание о существующих позициях. Strategy генерирует новые Open intents → pyramiding
> на existing position → 5× normal size (1537 SAGA / ~$45 notional) → liquidation.
> **Fix**: в `trading_pipeline.cpp` перед `execute()` блокируем Open intent если
> `has_open_position()` возвращает true. Бот scalp single position — pyramiding запрещён.
> **Config**: `hedge_recovery_enabled: false` (был true) — hedge recovery открывал opposite legs.
> Если нужна архитектурная поддержка hedge — pipeline state должен быть shared across
> DualLegManager instances; сейчас нет.
>
> **EDGE-27/28/29 (Late exit + profit lock + coverage 2026-05-15):**
> User-reported: winners закрываются поздно, отдавая profit; losers держатся 4-10min с -0.5%+.
> **EDGE-27** (`exit_orchestrator.cpp`): tiered fast adverse exits —
> hold<60s/-0.4%, hold<5min/-0.6%, hold<10min/-0.8%. Catches RIVER-type losses early.
> **EDGE-28** (`configs/production.yaml`): faster profit harvest —
> breakeven 0.4→0.3 ATR, partial_tp 2.0→1.5 ATR, partial_fraction 50→60%.
> **EDGE-29**: `top_n: 5 → 8` для broader market coverage.
>
> **EDGE-29-EXT (Session 5 continuous profitability loop 2026-05-15):**
> User-reported: run47 net=-0.0288 USDT/25min (realized +0.0166, fees 0.0453 — fees съели прибыль),
> залипание на 4 символах (SAGA/RIVER/Q/SKYAI), pipeline_count=2 при top_n=8.
> **Конфиг (`configs/production.yaml`)**:
> * `top_n: 8 → 12` (расширили watchlist coverage 1.4%→2.2%).
> * `max_spread_bps: 12 → 20` (больше pairs проходит pre-filter).
> * `scorer_volatility_low_threshold: 0.30 → 0.20` (допускаем менее волатильные pairs).
> * `max_pairs_per_sector: 5 → 8` (диверсификация секторов).
> * `min_volume_usdt: 1000000 → 800000`.
> * `atr_stop_multiplier: 1.0 → 1.4` (адаптация к trail_mult≈1.87).
> * `breakeven_atr_threshold: 0.3 → 0.5`, `partial_tp_atr_threshold: 2.0 → 2.5`, `partial_tp_fraction: 0.60 → 0.50`.
> * `min_risk_reward_ratio: 1.0 → 1.30` (нужен margin над 12 bps round-trip fees).
> **Validator constraint**: `rotation_interval_hours ≥ 0.05` (минимум 3 min) — hardcoded.
>
> **EDGE-29-EXIT-FIX (Session 5 hotfix 2026-05-15 после run48):**
> Run48 2 consecutive HYPEUSDT trades закрылись `continuation_value_exit` при PnL ~-0.011 USDT
> (cont_val=-0.117 и -0.159, exit_threshold ≈ -0.06). Структурная проблема: на микро-аккаунте
> $5-10 notional × 0.06% taker × 2 = 0.012 USDT fee = ~0.12% on price. Market noise сопоставим.
> **Патч `src/pipeline/exit_orchestrator.cpp:754,770`**:
> * `shallow_loss_floor: -round_trip_fee × 1.5 → × 3.0` — расширили dead zone до 3× fee.
> * `bearish gate threshold: cont_val > -0.15 → > -0.22` — требуем глубже падение для exit.
> Целевой эффект: continuation_exit не fires в economic noise band; trailing/structural exits
> остаются как защита от реального adverse.
>
> **Session 5 results (run47 → run48 → run49):**
> * run47 (25 min, original config): 4 closed trades, realized +0.017, fees 0.045, net **-0.028 USDT**.
> * run48 (12 min, EDGE-29 config): 2 HYPEUSDT trades, realized -0.024, fees 0.013, net **-0.036 USDT**.
> * run49 (T+0, EDGE-29 config + exit_orchestrator patch): рестарт с расширенной dead zone.
>
> **EDGE-30 Session 6 (run54-81 2026-05-15):**
> Полная переработка для FIXED-margin + dynamic leverage + active rotation.
>
> **EDGE-30-DYNCAP** (`main.cpp:281+`): на startup REST `/api/v2/mix/account/accounts` →
> override `config.trading.initial_capital`. Bot работает с real exchange balance.
>
> **EDGE-30-DYN** (`fetch_symbol_precision` в trading_pipeline.cpp:779+): извлекает `maxLever`
> per-symbol из `/api/v2/mix/market/contracts`. Сохраняется в `ExchangeSymbolRules.max_leverage`.
> Bot clamps requested leverage до exchange per-symbol max (например, SAGA 10×, RIVER 20×, CRV 75×).
>
> **EDGE-30-MARGIN-v4** (`portfolio_allocator.cpp:compute_size_v2`): убран partition capital
> на num_pipelines. Использует FULL capital. Target margin = 3% от total_capital ВСЕГДА.
>
> **EDGE-30-LEV-AUTO** (`trading_pipeline.cpp:4040+`): после compute_size, leverage **auto-bumped**
> если `actual_notional / leverage > target_margin`. Это даёт **FIXED 3% margin при любом leverage**
> (bot raises leverage до min_notional/target_margin = 11× минимум).
>
> **EDGE-30-LEV-v3** (config): min_leverage 5→11, max 75, regime levs 60/35/15 (trend/range/vol).
> С min 11× margin никогда не превышает 3%×$5/$11 = OK boundary.
>
> **EDGE-30-LIMIT** (`risk_types.hpp:186-188`): max_concurrent 5→10, max_long/short 3→8.
>
> **EDGE-30-SIGNAL-v3** (config): min_conviction_threshold 0.50→0.55 (balance активность/качество).
>
> **EDGE-30-CONT-v2** (`exit_orchestrator.cpp:check_continuation_value_exit`):
> threshold -0.22→-0.40, bearish_confirms >= 3 (раньше 2). Fewer false cont-exits.
>
> **EDGE-30-EXIT** (`exit_orchestrator.cpp:compute_trailing_stop`): trail_mult scales by
> profit_in_atr — больше profit → wider trail. Winners run.
>
> **EDGE-30-SCALP** (`trading_pipeline.hpp:kMinWarmupTicks`): 200→60 ticks (~1 min).
> Fast scalping start vs 3-5 min warmup.
>
> **EDGE-30-PROFIT-COMBO** (run81 audit): force TP 5%→10% margin ROI. Между:
> trailing stop с profit_f scale + partial_tp 4×ATR (33%) ловят runs. Force close ТОЛЬКО при extreme +10%.
>
> **EDGE-30-LOSSCUT** (run81 SAGA audit -0.015 EMA-cross): tier тighter:
> hold<60s/-0.5%, <5min/-0.7%, <10min/-0.9%, <30min/-1.2%. Catches mid-hold losses раньше EMA-cross.
>
> **EDGE-30-SOFT** (`main.cpp:838-844`): rotation НИКОГДА не replaces pipelines с open positions.
> Double-check pipelines[pi]->has_open_position() перед stop(). Explicit log "Soft rotation".
>
> **Session 6 results trajectory:**
> * run54 (1h20m): -0.31 USDT (reentry spiral, fast adverse cuts too aggressive)
> * run67 (1h): -0.10 (config-only fixes, leverage stuck=20)
> * run79 (30 min): +0.060 realized / -0.20 net (5 wins force TP $0.015 each + 1 loss $0.015)
> * run80-81 (interrupted): tested soft rotation + dynamic leverage 12-16× — auto-margin 2.8-2.9%
> * Stop equity 13.20 (started session at 16.13 ≈ -2.93 USDT за все runs от ошибок до calibration)
>
> **Известные ограничения сейчас:**
> 1. Spread/slippage съедают margin scalper trades — нужен maker/postonly orders (TBD)
> 2. Fees taker 0.06% × 2 = 0.12% round-trip требует >0.4% profit для break-even
> 3. WR должен быть >40% при R:R 4:1 — текущий ~80% но fees компенсируют
>
> **EDGE-26 (Pipeline rotation activation 2026-05-15):**
> Run44 evidence: rotation_interval=3min настроен, но pipelines не ротировались.
> Причина: `kIdleThresholdNs = 10 минут` в `main.cpp:682`. Pipeline считается idle
> только after 10min без activity. С tick-flow это никогда не срабатывало →
> 5 pipelines оставались static независимо от market conditions.
> **Fix**: `kIdleThresholdNs: 10min → 3min`, `kIdleCheckIntervalSec: 120 → 30s`,
> `kMinRescanIntervalNs: 10min → 3min`. Pipelines теперь активно ротируются
> когда не дают setups через 3 мин.
>
> **EDGE-25 (Scanner diversification fix 2026-05-15):**
> Run43 evidence: top_n=5 настроен, но scanner select=2 за каждый scan.
> Причина: `max_pairs_per_sector: 2` (default) + ВСЕ alt symbols классифицируются
> sector="other" → diversification cap 2/sector. HYPE, Q, TON, SAGA, NEAR — все
> blocked через "DIVERSIFICATION: сектор перегружен".
> **Fix**: `max_pairs_per_sector: 2 → 5` в `configs/production.yaml`. Все 5 top_n
> кандидатов из "other" сектора теперь спавнят pipelines.
> Effective coverage: 2 → 5 pairs (2.5× больше).
>
> **EDGE-24 (VPIN toxic threshold 2026-05-15):**
> Run42 evidence: 3/4 trades закрылись через VPIN toxic flow exit при near-zero pnl.
> Причина: `check_toxic_flow` exit при ЛЮБОЙ потере + vpin_toxic. VPIN flickers
> toxic на alts с low volume — false positive frequency высокая.
> **Fix** в `exit_orchestrator.cpp:382`: require `unrealized_pnl_pct < -0.3%`
> (meaningful loss) и hold ≥ 5min или loss > -0.5%.
>
> **EDGE-23 (Broader market coverage 2026-05-14):**
> Архитектурный bottleneck: top_n=3 = 3/553 pairs visible = 0.5% market coverage.
> Когда 3 selected pairs stagnant → bot idle 10-20 min до next scan rotation.
> **Fix**: `top_n: 3 → 5` (5/553 = 0.9%), `rotation_interval_hours: 0.08 → 0.05` (3 min).
> Combined effect: 5 параллельных pipelines + 3-min rotation = ~67% больше opportunities.
>
> **EDGE-22 (Architectural fix — high-conf bypass 2026-05-14):**
> Архитектурный root cause: 80% setups погибают в `setup_confirmation_window_ms`
> через `imbalance_reversed` за время ожидания. На scalping окне (sec-min) orderbook
> flips constantly — confirmation window 800ms = 30-50% setup death rate.
> **Fix** в `setup_lifecycle.cpp:495`: high-conf setups (conf > 0.85) bypass
> confirmation window и идут directly в EntryReady. Reduces detection-to-entry
> latency для best setups с 800ms+ до ~150ms.
> Логика: высокая conviction strategy means signal is robust against noise —
> waiting just kills good signals.
>
> **EDGE-21 (HTF Trend Filter calibration 2026-05-14):**
> Run39: 2/2 SELL setups blocked HTF filter (htf_strength=0.53 > threshold 0.15).
> Threshold 0.15 был "tighter" но реально блокировал почти любой counter-trend setup.
> **Fix** в `trading_pipeline.cpp:3269,3288`: `htf_trend_strength > 0.15 → 0.50`.
> Только STRONG counter-trend (>0.50) блокирует — умеренный allowed.
>
> **EDGE-20.2 (BB filter calibration 2026-05-14):**
> EDGE-20.1 `bb_min_sell=0.15` блокировал ВСЕ SELL setups в bear trend (bb_pos<0 = норма
> для downtrend, не exhaustion). Тоже для BUY: 0.85 слишком тесно.
> **Calibration**:
> - `rsi_upper_guard`: 70 → 72 (extreme overbought only)
> - `rsi_lower_guard`: 30 → 28 (extreme oversold only)
> - `bb_max_buy`: 0.85 → 1.10 (block только если bb_pos > upper BB)
> - `bb_min_sell`: 0.15 → -0.10 (block только при экстремальном lower BB)
> Lesson: BB filter не sole exhaustion indicator — в trend continuation bb_pos может
> долго быть на одной стороне.
>
> **EDGE-20 (Exhaustion entry filter 2026-05-14):**
> Run36 XRP entry анализ: bot купил @ 1.5417 при RSI=70.7, bb_pos=0.94 → -0.42% за 37 sec.
> "Buy the top" pattern — bot реагирует на завершённый импульс, нет exhaustion detection.
> **Fix** в `strategy_config.hpp:55-61`:
> - `rsi_upper_guard`: 80 → 65 (block BUY если RSI overbought)
> - `rsi_lower_guard`: 20 → 35 (block SELL если RSI oversold)
> - `bb_max_buy`: 1.50 → 0.80 (block BUY если price у upper Bollinger)
> - `bb_min_sell`: -0.50 → 0.20 (block SELL если price у lower Bollinger)
> Без этого filter bot покупал exhausted moves — instant reversal → fast adverse exit -$0.026.
>
> **EDGE-19 (Opportunity flow recovery 2026-05-14):**
> После EDGE-17 fast adverse exit bot простаивал 16 мин до next setup. Причина:
> 5-min symbol cooldown + 10-min scanner rotation interval.
> **Fix**:
> - `max_consecutive_losses_per_symbol`: 1 → 2 (1 loss = шум, не блокируем)
> - `symbol_cooldown_after_stopouts_ns`: 5 min → 2 min
> - `rotation_interval_hours`: 0.17 (10min) → 0.08 (5min)
>
> **EDGE-18 (Side balance fix 2026-05-14):**
> Диагностика run35 показала: BUY 127 setups, SELL 7 setups (18× bias). При этом
> ВСЕ 7 SELL setups блокированы pipeline через R:R 1.0 < min_rr 1.2.
> **Причина**: strategy R:R = (atr × partial_tp_atr_threshold × reward_boost) / (atr × stop_mult).
> reward_boost получает +1.25× при ADX>35 и +1.20× при HTF-aligned trend direction.
> На bear EMA BUY получает только +1.20× (HTF aligned), SELL не получает оба boost'а
> в большинстве случаев (низкий ADX в downtrend). Result: BUY R:R = 1.2-1.5, SELL R:R = 1.0.
> **Fix**: `min_risk_reward_ratio: 1.2 → 1.0` в `configs/production.yaml:76`. Breakeven WR 50% — приемлемо.
>
> **EDGE-17 (Fast-reaction scalping 2026-05-14):**
> Entry latency была 3.6 сек (setup → entry confirmation), что слишком медленно для scalping:
> orderbook flips в этом окне → 54/126 SELL setups отменяются imbalance_reversed.
> **Fixes**:
> 1. `setup_confirmation_window_ms`: 3000 → 800 ms (3.75× faster)
> 2. `setup_timeout_ms`: 15000 → 8000 ms
> 3. `cooldown_after_exit_ms`: 5000 → 3000 ms; `cooldown_after_failed_setup_ms`: 3000 → 2000 ms
> 4. **Signal freshness gate** в `strategy_engine.cpp:373`: если `now_ns - setup->detected_at_ns > 2 сек` — invalidate, не входить на устаревшем сигнале
> 5. **Fast adverse exit** в `exit_orchestrator.cpp:27`: hold<60s + `unrealized_pnl_pct < -0.4%` → немедленный exit (urgency 0.95). Срабатывает на failed continuation после entry до того как trailing/SL очнутся.
> **Result**: entry latency 3600 → 964 ms (3.7× быстрее) измерено в run35.
>
> **SCALPING REFACTOR (2026-05-16) — обзор:**
> Снят слой institutional-сложности после серии EDGE-калибровок. Удалено и переписано:
>
> 1. **Удалены модули** (нулевые внешние ссылки): `src/ml/regime_ensemble`, `src/ml/meta_label`, `src/ml/calibration` (~1387 LOC + тесты). Импортировались только из удалённого `regime_ensemble`.
> 2. **Удалён `src/strategy_allocator/`** (~373 LOC + тесты). Был degenerate при одной активной стратегии (`scalp_engine`). Типы `AllocationResult`/`StrategyAllocation` переехали в `src/decision/strategy_allocation.hpp`. Расчёт веса — inline в `trading_pipeline.cpp:2911+`.
> 3. **`src/world_model/` переписан** (1998 → 722 LOC, −64%). 9-state адаптивная машина с гистерезисом, transition matrix 9×9, feedback-EMA, multi-driver scoring и audit-trail удалена. Новый engine — pure classifier по 5 порогам, без internal state-tracking за пределами `last_[symbol]`. `world_model_history.hpp` удалён.
> 4. **`src/uncertainty/` переписан** (1134 → 539 LOC, −52%). 9-мерная композитная модель → 4 hard-сигнала (spread/data_fresh/VPIN/book_instability) + 3 soft-сигнала (regime confidence/world fragility/ML cascade-or-correlation). Aggregate: `0.7×max(hard) + 0.3×avg(soft)` с EMA-сглаживанием.
> 5. **`src/decision/` surgical cleanup.** Удалены BUY/SELL conflict resolution, ensemble bonus, `compute_regime_threshold_factor`, `compute_regime_dominance_threshold`. Conviction threshold формула переписана:
>    `threshold = base + min(0.25, severity*0.30) + drawdown_boost + min(0.20, total_cost_bps/200)`, `severity = max(uncertainty_level_severity, danger_prob*0.7)`, hard cap 0.80 (раньше 0.90 — был часто недостижим).
> 6. **`src/leverage/leverage_engine.cpp:uncertainty_multiplier`** нейтрализован: Low/Moderate/High → 1.0, Extreme → 0.6. Раньше Moderate→0.80, High→0.55, Extreme→0.25 — это double-counted с `uncertainty.size_multiplier` в pipeline allocator и сжимало notional ниже min_notional на $15-аккаунте.
> 7. **Maker-first execution path** (для устранения fee drag):
>    * `strategy_engine.cpp:build_intent` — `intent.urgency = 0.30` (раньше 0.9). Это позволяет execution_alpha выбрать PostOnly через EV-модель.
>    * `execution_planner.cpp:choose_style` — `ExecutionStyle::PostOnly` и `Passive` теперь маппятся в `PlannedExecutionStyle::PostOnlyLimit` (раньше → `PassiveLimit` без `force=post_only` → терялась maker-гарантия).
>    * `fill_processor.cpp` — fee по `order_type`: maker (2 bps) для PostOnly/Limit/StopLimit, taker (6 bps) для Market/StopMarket. Раньше всегда taker → искажённый PnL для maker fills.
>    * `production.yaml` `urgency_aggressive_threshold` 0.50 → 0.80; `postonly_spread_threshold_bps` 8 → 12.
> 8. **Size-multiplier стек обрезан до single source.** Pipeline allocator (`trading_pipeline.cpp:2911+`): только `uncertainty.size_multiplier` масштабирует size. Параллельные множители `regime.hint.weight_multiplier` и `world.strategy_suitability` удалены. Regime сохранил **hard-veto** (`should_enable=false` → reject), но soft-multiplier path вырезан.
>
> **Объёмы:** ~−3500 LOC src/ + ~−750 LOC устаревших тестов = ~−4250 LOC чистого сокращения. Build clean, 631/631 тестов pass.
>
> **EDGE-калибровки EDGE-1..30 сохранены без изменений.** Refactor only удалил institutional-overengineering сверх калибровок — не калибровал thresholds сам.

---

## 1. Обзор системы

### 1.1 Контекст

Tomorrow Bot — узкоспециализированная торговая система для одного семейства инструментов (perpetual futures USDT-M), работающая исключительно с биржей Bitget (REST v2 + private/public WebSocket). Проектные ограничения:

* low-latency (целевой бюджет на принятие решения по тику не задан явно в коде; косвенные оценки P50/P95/P99 встроены в `PipelineLatencyTracker`)
* фоновая эксклюзивная торговля (один процесс, signed-singleton supervisor)
* hedge-mode + isolated margin, что вынуждает дисциплину по `position_side ∈ {Long, Short}` и `tradeSide ∈ {Open, Close}` на уровне submitter'а
* единственная стратегия (скальпинг — `StrategyId("scalp_engine")`). Из 4 сетапов на production активен только `MomentumContinuation`; `Retest`, `PullbackInMicrotrend`, `Rejection` остаются disabled-by-default через `ScalpStrategyConfig::enable_*_scenarios = false` (EDGE-13/EDGE-15/EDGE-29).
* малый капитал (~$15 → $50): архитектура оптимизирована под низкий fee drag (maker-first PostOnly), а не под institutional-scale risk management.

### 1.2 Технологическая база

* C++23 (`-std=c++23`, `CXX_EXTENSIONS=OFF`)
* Boost.Asio + Boost.Beast (REST/WebSocket клиенты Bitget)
* Boost.JSON (парсинг payload'ов биржи и журнала)
* PostgreSQL (libpqxx) — `persistence` слой (snapshot + WAL)
* OpenSSL (HMAC-SHA256 для подписи Bitget API + TLS)
* Catch2 v3 (FetchContent) — тестовый фреймворк, 631 тестов (2026-05-16 после scalping refactor)

### 1.3 Режимы работы

В коде определён единственный `enum TradingMode { Production }`. Ранние режимы (Paper/Shadow) были удалены — `paper`/`shadow` теперь существуют только как значения `mode:` в `production.yaml`, эмулируемые на уровне submitter'а (см. `staged_rollout.yaml`).

Запуск Production требует:

1. Корректные ключи в `.env` или env vars (`BITGET_API_KEY`, `BITGET_API_SECRET`, `BITGET_PASSPHRASE`).
2. `TOMORROW_BOT_PRODUCTION_CONFIRM=I_UNDERSTAND_LIVE_TRADING` (валидируется `ProductionGuard`).
3. URL биржи в allowlist Bitget production endpoints.
4. Хеш конфигурации фиксируется в логах для аудита (`config_hash` ⊂ runtime manifest).

---

## 2. Архитектура (текстовое представление, по слоям)

### 2.1 Слои (сверху вниз)

| № | Слой | Каталоги |
|---|------|----------|
| 0 | Bootstrap / lifecycle | `app/`, `supervisor/` |
| 1 | Конфигурация / безопасность | `config/`, `security/` |
| 2 | Инфраструктура | `logging/`, `metrics/`, `clock/`, `common/`, `buffers/`, `resilience/`, `persistence/` |
| 3 | Биржа | `exchange/bitget/` |
| 4 | Сырые/нормализованные данные | `market_data/`, `normalizer/`, `order_book/` |
| 5 | Технические признаки | `indicators/`, `features/` |
| 6 | Доменные модели | `world_model/`, `regime/`, `uncertainty/` |
| 7 | Принятие решений | `strategy/` (+ `setups`, `state`, `context`, `management`), `decision/`, `ml/` |
| 8 | Сайзинг и риск | `portfolio/`, `portfolio_allocator/`, `risk/` (+ `policies`, `state`), `leverage/`, `opportunity_cost/`, `execution_alpha/` |
| 9 | Исполнение | `execution/` (+ `orders`, `planner`, `fills`, `cancel`, `recovery`, `telemetry`) |
| 10 | Сверка / восстановление | `reconciliation/`, `recovery/` |
| 11 | Сканирование пар | `scanner/` |
| 12 | Оркестратор | `pipeline/` |
| 13 | Наблюдаемость | `telemetry/`, `cost_attribution/` |
> **Defect-D1 (HIGH) — ЗАКРЫТ 2026-05-07.** Пять orphan-каталогов (`adversarial_defense`, `alpha_decay`, `drift`, `self_diagnosis`, `validation`) удалены вместе с orphan-тестами. Их функциональность либо реализована inline в pipeline (adversarial_severity), либо не была обещана README. Текущее состояние — 39 модулей, ноль orphan'ов.

### 2.2 Ключевая декомпозиция оркестрации

```
[main()] → [AppBootstrap]
               ↓
       [ScannerEngine.scan()] ← Bitget REST (public)
               ↓ (top-N символов)
   ┌─── [Supervisor] ──── (sym_locks, kill_switch, position_count)
   │
   │   per symbol:
   │   ┌── [TradingPipeline]
   │   │   ├── [BitgetWsClient] → [Normalizer] → [LocalOrderBook]
   │   │   │                                      └→ [FeatureEngine]
   │   │   ├── [WorldModel] [Regime] [Uncertainty]
   │   │   ├── [StrategyEngine] (setup detect/validate, position manage)
   │   │   ├── [DecisionEngine]
   │   │   ├── [ml/*] (Bayesian, Thompson, Entropy, Cascade, Correlation, Fingerprint)
   │   │   ├── [PortfolioAllocator] [OpportunityCost] [ExecutionAlpha]
   │   │   ├── [LeverageEngine] [RiskEngine]
   │   │   └── [ExecutionEngine] → [BitgetFuturesOrderSubmitter] → REST
   │   │       └── [OrderWatchdog] [ExitOrchestrator] [HedgePairManager] [DualLegManager]
   │   ├── [BitgetPrivateWsClient] (event-driven fills)
   │   └── [ReconciliationEngine] (60s runtime, ∞ при mismatch)
   │
   └── [PersistenceLayer] (snapshot 30s + WAL events)
```

Каждый `TradingPipeline` создаётся **на один символ** и владеет полным стеком модулей слоёв 4-10, кроме `IPortfolioEngine`, `IMetricsRegistry`, `ILogger`, `IClock` и `ISecretProvider`, которые передаются через `shared_ptr` и являются процесс-уровневыми.

---

## 3. Карта модулей

### 3.1 Боевые (in-build) модули, 36 шт.

| Каталог | Назначение (одной строкой) |
|---------|---------------------------|
| `app` | Точка входа, парсинг CLI, bootstrap, HTTP-сервер метрик, runtime manifest |
| `common` | Strong types (`Symbol`/`Price`/`Quantity`/...), `Result<T>`, `TbError`, exchange-rules, reason codes |
| `config` | Загрузчик YAML (собственный flat-парсер), `AppConfig`, валидатор, `config_hash` (SHA-256) |
| `security` | `ISecretProvider` (env/file), `ProductionGuard`, redaction, credential types |
| `logging` | `ILogger`, JSON-форматтер, контекст, события, sink-ы (console/file/composite) |
| `metrics` | `IMetricsRegistry`, counter/gauge/histogram, Prometheus-экспорт, теги |
| `clock` | `IClock` (system/test), `WallClock`, утилиты timestamp |
| `supervisor` | `Supervisor` — жизненный цикл, sigaction, symbol-locks, kill-switch broadcast, global position limit |
| `exchange/bitget` | REST v2 (mix), public WS, private WS, signing (HMAC-SHA256), futures order submitter, query adapter |
| `normalizer` | Bitget JSON → `NormalizedTicker/Trade/OrderBook/Candle`, фильтрация non-futures, sequence assignment |
| `market_data` | `MarketDataGateway` — связывает WS + Normalizer + OrderBook + FeatureEngine |
| `order_book` | Локальный L2 стакан (sorted maps), snapshot+delta, sequence continuity, crossed-book detection, event subscribers |
| `buffers` | Кольцевые буферы свечей и трейдов (template `<size>`) |
| `indicators` | SMA/EMA/RSI(Wilder)/MACD(Appel)/BB/ATR/ADX(Wilder)/OBV/VWAP/ROC/Z-Score/Volatility/Momentum |
| `features` | `FeatureEngine` + `AdvancedFeatureEngine` (CUSUM, VPIN, Volume Profile, Time-of-Day, Microstructure) |
| `world_model` | Минимальный 5-state классификатор (Stable/Disrupted/Transitioning labels), `strategy_suitability` для активного `scalp_engine`, `state_probabilities` для downstream danger-penalty. **Без гистерезиса/transition-matrix/feedback** (удалены в scalping refactor). |
| `regime` | 13 detailed regimes, hysteresis (dwell/candidate ticks), CUSUM-ускорение, regime hints |
| `uncertainty` | 4-сигнальный gate (spread/data_fresh/VPIN/book_instability) + 3 soft-сигнала (regime confidence, world fragility, ML cascade/correlation). Производит `size_multiplier` и severity для conviction threshold. **9-мерная композитная модель удалена.** |
| `strategy` | Скальпинг: state machine, setup detector/validator, position manager. Из 4 сетапов на production активен только `MomentumContinuation` (EDGE-13/EDGE-15/EDGE-29). `intent.urgency = 0.30` для entry → maker path. |
| `decision` | Bounded intent-gate: severity-max (max-of-priorities, не sum) + drawdown_boost + cost_headroom; hard cap 0.80. **BUY/SELL conflict / ensemble bonus / regime threshold factor удалены** (committee voting degenerate при N=1). Содержит `decision/strategy_allocation.hpp` с типами, ранее находившимися в `strategy_allocator/`. |
| `portfolio` | `IPortfolioEngine` (in-memory) — позиции, hedge-mode, cash ledger, pending orders, аудит-лог, инварианты |
| `portfolio_allocator` | `HierarchicalAllocator` — concentration/budget/strategy limits, vol targeting, Kelly fraction, drawdown scaling, liquidity caps |
| `risk` | `ProductionRiskEngine` — 33 policy checks (kill switch, locks, daily loss, drawdown, exposure, concentration, funding, ...) |
| `execution` | `ExecutionEngine` + 6 подсистем (registry, planner, fill_processor, cancel_manager, recovery_manager, exec_metrics), FSM, TWAP. Планировщик маппит `ExecutionStyle::PostOnly` в `PlannedExecutionStyle::PostOnlyLimit` → реальный maker. `FillProcessor` различает maker/taker fee rate по `order_type`. |
| `execution_alpha` | Maker/taker/PostOnly выбор, urgency, fill prob, VPIN, queue-aware adjustments, EV-based выбор стиля |
| `opportunity_cost` | Net edge in bps, defer/execute/upgrade/suppress, drawdown penalty, consecutive loss penalty |
| `leverage` | Адаптивное плечо: 7 множителей (regime, vol, drawdown, conviction, funding, adversarial, uncertainty), Kelly cap, EMA smoothing. **`uncertainty_multiplier` нейтрализован для Low/Moderate/High** (был double-counted с `uncertainty.size_multiplier`); подавляет leverage только при Extreme (0.6). После `EDGE-30-LEV-AUTO` pipeline сам поднимает leverage до 11+ для фиксации 3% margin. |
| `reconciliation` | Сверка ордеров/позиций/баланса с биржей, mismatch resolution, periodic + startup |
| `recovery` | `RecoveryService` — восстановление позиций/баланса при старте, snapshot+WAL replay, polished extended recovery |
| `resilience` | `CircuitBreaker`, `IdempotencyManager` (client_order_id), `RetryExecutor`, `OperationalGuard` (auto reduce-risk) |
| `persistence` | Фасад: `EventJournal` + `SnapshotStore`, in-memory и Postgres адаптеры, WAL-writer |
| `scanner` | Топ-N выбор пар: features → traps → filter → ranking → bias, 7 trap-детекторов, диверсификация |
| `ml` | Bayesian adapter, Thompson sampling, entropy filter, microstructure fingerprint, liquidation cascade, correlation monitor, regime ensemble, calibration, meta-label |
| `telemetry` | `ResearchTelemetry`, sinks (file/memory), incident detector, observability panels |
| `cost_attribution` | Атрибуция cost-компонентов (fees, slippage, spread, queue, adversarial) |
| `pipeline` | `TradingPipeline` (главный класс ~580 строк состояния), watchdog, exit orchestrator, hedge pair manager, dual-leg manager, market reaction engine, latency tracker |

### 3.2 Удалённые orphan-модули (история)

В рамках рефакторинга 2026-05-07 удалены: `adversarial_defense`, `alpha_decay`, `drift`, `self_diagnosis`, `validation`. Подробное обоснование — в `docs/refactor_plan.md` (раздел про D1). Pipeline продолжает торговлю; adversarial detection реализован inline (см. `trading_pipeline.cpp:3849-3921`).

---

## 4. Потоки данных

### 4.1 Входной hot-path (тик → ордер)

```
Bitget WS frame (text)
  ─→ BitgetWsClient::on_message (single io_context thread)
  ─→ MarketDataGateway::on_raw_message
  ─→ BitgetNormalizer::process_raw_message  (фильтр non-futures, валидация)
  ─→ NormalizedEvent (Ticker/Trade/OrderBook/Candle)
  ─→ либо LocalOrderBook (snapshot/delta, sequence check),
     либо FeatureEngine (on_trade/on_ticker/on_candle, обновление buffers)
  ─→ FeatureEngine::compute_snapshot(symbol, book) → FeatureSnapshot
  ─→ MarketDataGateway::FeatureSnapshotCallback
  ─→ TradingPipeline::on_feature_snapshot(snapshot)        ← граница тика
        WorldModel::update              → WorldModelSnapshot
        RegimeEngine::classify          → RegimeSnapshot
        UncertaintyEngine::assess(...)  → UncertaintySnapshot
        StrategyEngine::evaluate        → optional<TradeIntent>
        StrategyAllocator::allocate     → AllocationResult
        DecisionEngine::aggregate       → DecisionRecord
        ml/* feedback                   → MlSignalSnapshot
        PortfolioAllocator::compute_size_v2 → SizingResult
        ExecutionAlpha::evaluate        → ExecutionAlphaResult
        OpportunityCost::evaluate       → OpportunityCostResult
        LeverageEngine::compute_leverage → LeverageDecision
        RiskEngine::evaluate            → RiskDecision
        ExecutionEngine::execute(...)   → Result<OrderId>
        BitgetFuturesOrderSubmitter::submit_order → REST POST
```

Каждая стадия может вернуть `veto`/`Skip`/`Deny`, тогда тик завершается без ордера и фиксируется в diag-counter (см. `TradingPipeline::diag_*_block_`).

### 4.2 Pull-pипa (периодические задачи)

| Задача | Период | Поток |
|--------|--------|-------|
| Reconciliation активных ордеров | 60 с | hot path (внутри тика) |
| Reconciliation позиций+баланс | 5 мин | hot path |
| Snapshot портфеля → persistence | 30 с | hot path |
| Reference prices BTC/ETH (для CorrelationMonitor) | 30 с | `std::jthread ref_price_thread_` |
| Watchdog ордеров | 10 с | hot path |
| HTF (1h) обновление | 60 мин или urgent | hot path (REST) |
| Funding rate update | 5 мин | hot path |
| Incident check | 10 с | hot path |
| Pair rotation (ScannerEngine) | 24 ч (config) | rotation thread |

> **Defect-D2 (HIGH).** Большинство периодических задач исполняется в hot-path тика. На связке REST-вызов внутри `run_continuous_reconciliation` блокирует обработку WS-фрейма на сотни миллисекунд. Это латентный bottleneck (раздел 7).

### 4.3 Event-driven fill path

```
Bitget Private WS (orders.{instId})
  ─→ BitgetPrivateWsClient::on_msg (own io_context thread)
  ─→ TradingPipeline::on_private_ws_message(channel, data)   ← @MUTEX pipeline_mutex_
        ExecutionEngine::on_fill_event / on_order_update
        FillProcessor → portfolio.open_position / reduce_position
        Strategy::notify_position_opened/closed
        LeverageEngine::update_edge_stats (через trade_history_)
```

> **Inv-9 (см. § 8).** Private WS поток и hot-path WS поток входят в `TradingPipeline::pipeline_mutex_`. Если оба борются за лок, latency public-feed увеличивается. Контракт: hot-path должен освобождать мьютекс на ≤ N мс между шагами или использовать lock-free очередь.

---

## 5. Потоки управления

### 5.1 Жизненный цикл

```
main() → AppBootstrap.initialize()
         → config_loader.load() → AppConfig
         → secret_provider (env|file)
         → logger (console + опционально file → composite)
         → metrics_registry
         → clock (WallClock)
         → ProductionGuard.validate() ← guard-Pre  (см. § 8 Inv-1)
       (kill локальных копий ключей в bootstrap)
       ↓
       HttpEndpointServer (если metrics.enabled): :PORT/PATH
       ScannerEngine.scan() ← ретраи до 30 минут на пустой результат
       ↓
       per symbol → TradingPipeline(...) + Supervisor.register_subsystem(...)
       Supervisor.start() (FIFO start, LIFO stop)
       Supervisor.install_signal_handlers() (SIGTERM, SIGINT)
       Supervisor.wait_for_shutdown()
       Supervisor.stop() с таймаутом 30 с
```

### 5.2 Иерархия мьютексов (deadlock prevention)

В `supervisor.hpp` явно зафиксирован порядок:

```
state_mutex_ → symbol_lock_mutex_ → kill_switch_mutex_ → positions_mutex_
```

Внутри `TradingPipeline` дополнительно:

```
pipeline_mutex_ (top)  →  все per-engine mutex'ы (RiskEngine::mutex_, OrderRegistry::mutex_, …)
```

> **Inv-10.** Захватывать чужой мьютекс под `pipeline_mutex_` допустимо только для движков того же `TradingPipeline`. Связи с `Supervisor` строятся **без** удержания `pipeline_mutex_`, иначе есть риск инверсии.
> Контроль: запрещено вызывать `Supervisor::*` под `pipeline_mutex_`.

### 5.3 Kill-switch broadcast

`Supervisor::activate_global_kill_switch(reason)` — атомарно ставит флаг и итерирует listener'ов. Каждый pipeline регистрируется как listener и в callback:

* помечает `RiskEngine::activate_kill_switch(reason)`
* блокирует новые входы (но НЕ выходы)
* инициирует отмену активных лимиток через `ExecutionEngine::cancel_all_for_symbol`

> **Defect-D3 (MEDIUM).** В коде нет явного проброса `Supervisor::is_kill_switch_active()` в `RiskEngine::evaluate(...)`. RiskEngine хранит **локальный** флаг (`kill_switch_active_`), который синхронизируется через listener. Между переходом и применением окно, в котором `RiskEngine` может одобрить ордер, а submitter — отправить его. Решение: в `RiskEngine::evaluate` выполнять `bool ks = supervisor->is_kill_switch_active() || kill_switch_active_.load();` или сделать `ProductionRiskEngine` consumer'ом event bus и обновлять флаг до начала evaluate.

---

## 6. Конкурентная модель

### 6.1 Потоки, существующие в системе

| Поток | Источник | Назначение |
|-------|----------|------------|
| Main | `main()` | bootstrap, scanner, ожидание shutdown |
| Public WS io_context | `BitgetWsClient::Impl` | приём публичных market-data фреймов |
| Private WS io_context | `BitgetPrivateWsClient` | приём fill/position событий |
| Scanner rotation | `ScannerEngine::rotation_thread_` | пере-сканирование top-N каждые 24 ч |
| Reference price fetch | `TradingPipeline::ref_price_thread_` (`std::jthread`) | фоновый запрос BTC/ETH |
| HTTP metrics endpoint | `HttpEndpointServer` | Prometheus exposition |
| Signal handler | OS-thread (async-signal-safe) | `Supervisor::signal_handler` → атомарный CAS |

### 6.2 Гарантии

* `std::atomic` использован для `running_`, `state_`, `kill_switch_active_`, всех latency/diag-счётчиков.
* `std::mutex` — для всех stateful-структур (positions, locks, FSMs, registries).
* `std::shared_ptr` для процесс-уровневых синглтонов (logger, clock, metrics, secret_provider).
* `std::unique_ptr` для эксклюзивно владеемых ресурсов (`io_context`, watchdog).

### 6.3 Известные риски конкурентности

| Риск | Где | Тяжесть |
|------|-----|---------|
| Дедупликация TOCTOU | `ExecutionEngine::execute` без `execute_mutex_` была подвержена; зафиксировано через `mutex_` (комментарий «H-4: serialize execute() to prevent dedup TOCTOU») | устранено |
| Concurrent reconciliation | `ReconciliationEngine` использует отдельный `op_mutex_` для сериализации (BUG-S4-03) | устранено |
| `pipeline_mutex_` блокирует public WS | hot-path выполняет REST под мьютексом | **активно** |
| `BitgetRestClient` — потокобезопасность connection pool | `conn_pool_mutex_` сериализует pool, но не сериализует in-flight call | приемлемо |
| `LocalOrderBook::subscribers_` | список callback'ов не защищён — добавление subscriber во время `emit_events` | подозрительно (см. § 9) |

---

## 7. Узкие места

### 7.1 Латентность

* **Bottleneck-1 (CRITICAL).** REST-вызовы внутри hot-path тика:
  - `run_continuous_reconciliation` (60 с период, но при необходимости каскад REST)
  - `sync_balance_from_exchange` (5 мин)
  - `bootstrap_htf_candles` / `maybe_update_htf` (60 мин или urgent)
  - `fetch_symbol_precision` (one-shot, но при rotate — повторно)
  - При сетевом jitter 200-1000 мс это блокирует обработку всех WS-сообщений и нарушает порядок последовательностей в `LocalOrderBook`.
  - **Митигация:** вынести все REST в фоновые `std::jthread` с очередью результатов (как уже сделано для `update_reference_prices`).

* **Bottleneck-2 (HIGH).** Сериализация `pipeline_mutex_` между public-feed и private-feed: один большой замок на любой тип события.
  - **Митигация:** разделить на два мьютекса (`market_state_mutex_` для public, `execution_state_mutex_` для private) и аккуратно перепроверить инварианты (Inv-9).

* **Bottleneck-3 (MEDIUM).** `BitgetNormalizer` парсит JSON через Boost.JSON под общим `symbols_mutex_`. На пиковом потоке тиков (Bitget ~50-200 фрейм/с на инструмент) аллокации `std::string` в payload-парсинге доминируют. Mitigation: pool-allocator + reuse `boost::json::value`.

* **Bottleneck-4 (MEDIUM).** `ScannerEngine::scan()` последовательно опрашивает REST для всех 544 контрактов:
  - tickers (1 запрос),
  - orderbook на каждый прошедший prefilter (~30 запросов),
  - candles на каждый прошедший detail-filter (~5-7 запросов),
  - retry до 30 минут на пустой результат (`std::this_thread::sleep_for(60s)` в main).
  - **Митигация:** параллельные REST + переключение на batch-эндпоинты, где возможно. Приоритет — низкий (стартовый и периодический процесс).

### 7.2 Аллокации / копирования

* `OrderRegistry::orders_for_symbol` возвращает `std::vector<OrderRecord>` копированием — для 100+ ордеров это десятки KB на вызов внутри hot-path (опасно для cancel_timed_out).
* `std::map<Price, Quantity>` в `LocalOrderBook` — кэш-холодная структура; для 100-уровневого стакана при изменении выгоднее плоский вектор `pair<double,double>` сортированный (или intrusive RB-tree). При ETH/USDT с активным потоком обновлений `apply_delta` доминирует в перформансе.
* `FeatureEngine::compute_snapshot` пересчитывает индикаторы каждый тик из свежих буферов — нет инкрементальных индикаторов (см. § 9).

### 7.3 Блокирующие места

* `std::this_thread::sleep_for(60s)` в `main.cpp` (retry scanner).
* `RetryExecutor::execute<F>` — `std::this_thread::sleep_for(delay)` блокирует поток, в котором был вызван retry. Если это hot-path — блокировка `pipeline_mutex_` на весь backoff.
* `BitgetRestClient::wait_for_rate_limit` под `rate_mutex_` использует busy-wait/sleep при отсутствии токенов — также блокирующий.

---

## 8. Инварианты системы (formal)

### 8.1 Жёсткая классификация

* **Hard (нарушение → kill switch / немедленный halt):** Inv-1 ÷ Inv-12.
* **Soft (нарушение → лог + деградация):** Inv-S1 ÷ Inv-S8.

### 8.2 Системные инварианты

#### Inv-1 (HARD) — Production preflight

**Формулировка.** ∀ запуск с `mode=Production`:
`ProductionGuardResult.allowed = true` ∧ `env(TOMORROW_BOT_PRODUCTION_CONFIRM) == "I_UNDERSTAND_LIVE_TRADING"` ∧ `is_production_api(endpoint_rest) = true` ∧ `len(api_key)>0 ∧ len(api_secret)>0 ∧ len(passphrase)>0`.  
**Где проверяется.** `AppBootstrap::initialize` (`production_guard.cpp`).  
**При нарушении.** `Err(TbError::ProductionGuardFailed)`, exit с кодом ≠0.

#### Inv-2 (HARD) — Симметрия позиции и стороны

**Формулировка.** Для любой `Position p` в `IPortfolioEngine`:
`(p.side == Buy → p.position_side == Long) ∧ (p.side == Sell → p.position_side == Short)`.  
**Где проверяется.** `InMemoryPortfolioEngine::open_position` (должно), `Reconciliation::reconcile_positions` (по факту биржи).  
**При нарушении.** Hard-deny в `RiskEngine` + lock символа.

#### Inv-3 (HARD) — Идемпотентность fills

**Формулировка.** ∀ `OrderId o`: `fill_event(o)` применяется к `IPortfolioEngine` ровно один раз. `OrderRegistry::is_fill_applied(o) ⇒ ¬apply_again`.  
**Где.** `OrderRegistry::mark_fill_applied`, `FillProcessor`.  
**При нарушении.** Нарушение P&L → `kill_switch`.

#### Inv-4 (HARD) — ClientOrderId уникален в окне дедупликации

**Формулировка.** ∀ `client_order_id c` в окне `IdempotencyConfig::dedup_window`: `c` отправляется на биржу не более одного раза.  
**Где.** `IdempotencyManager::is_duplicate ∧ mark_sent`.  
**При нарушении.** `TbError::IdempotencyDuplicate`.

#### Inv-5 (HARD) — Cash conservation

**Формулировка.** В любой момент `available_cash + reserved_for_orders + Σ position_margin = total_capital_after_pnl + accumulated_funding − fees_accrued_today`.  
**Где.** `InMemoryPortfolioEngine::check_invariants`, периодически в `pipeline`.  
**При нарушении.** Hard-halt + `RecoveryService::recover_positions`.

#### Inv-6 (HARD) — FSM monotonicity ордера

**Формулировка.** Переход состояния `OrderState` следует разрешённой матрице FSM (`order_fsm.cpp::transition`); `force_transition` допустим только из `RecoveryManager` под чётким reason.  
**Где.** `OrderFSM::transition`.  
**При нарушении.** `Result<...>::Err`.

#### Inv-7 (HARD) — Sequence continuity стакана

**Формулировка.** Для `LocalOrderBook`: `delta.sequence > last_sequence_` (строго +1 без пропусков на Bitget) ⇒ `apply_delta`. Иначе → `request_resync()`.  
**Где.** `LocalOrderBook::apply_delta`.  
**При нарушении.** `BookQuality::OutOfSync` → стратегии не торгуют до resync.

#### Inv-8 (HARD) — Kill switch domination

**Формулировка.** ∀ `RiskDecision d`: если `Supervisor::is_kill_switch_active() = true` или `RiskEngine::is_kill_switch_active() = true` → `d.allowed = false ∧ d.action = EmergencyHalt`.  
**Где.** `KillSwitchCheck::evaluate` (первая в цепочке 33 проверок).  
**При нарушении.** см. Defect-D3.

#### Inv-9 (HARD) — Single owner ордера в registry

**Формулировка.** ∀ `OrderId o`: ровно одна запись в `OrderRegistry`, идентифицируемая по `o`. `register_order` ⇒ `o ∉ registry`. `orders_by_exchange_id` поддерживает биекцию между `exchange_id` и `OrderId`.  
**Где.** `OrderRegistry::register_order`.  
**При нарушении.** `Result<...>::Err` + лог.

#### Inv-10 (HARD) — Mutex hierarchy

**Формулировка.** Захват в порядке: `state → symbol_lock → kill_switch → positions → pipeline → engine_mutexes`. Никогда не вызывать `Supervisor::*` под `pipeline_mutex_`.  
**Где.** Контракт; верификация — code review + sanitizer (TSAN).  
**При нарушении.** Возможен deadlock.

#### Inv-11 (HARD) — Никаких ордеров без leverage/margin-mode setup

**Формулировка.** Перед первым `submit_order` для `symbol s`: `BitgetFuturesOrderSubmitter::set_leverage(s, lev, "long" ∧ "short") ∧ set_margin_mode(s, "isolated") ∧ set_hold_mode("USDT-FUTURES", "double_hold")` должны быть успешно выполнены.  
**Где.** `TradingPipeline::start` (на основании конфига).  
**При нарушении.** Bitget вернёт ордер → exchange-rejection. Pipeline должен отклонять до отправки (см. § 9 Defect-D4).

#### Inv-12 (HARD) — Reconciliation freshness

**Формулировка.** `now − last_reconciliation_ns ≤ 2× reconciliation_interval` для активной торговли. Если timeout — `OperationalGuard::HaltTrading`.  
**Где.** `TradingPipeline::run_continuous_reconciliation`.

### 8.3 Soft инварианты

* **Inv-S1.** `feed_freshness ≤ feature_engine.feed_freshness_ns`. Иначе `FeatureSnapshot.is_stale = true`, стратегии воздерживаются.
* **Inv-S2.** `LocalOrderBook.depth ≥ 5 уровней`. Иначе `BookQuality::Sparse`, `RiskCheck::ThinOrderBook` блокирует.
* **Inv-S3.** `regime_engine.confidence ≥ 0.30`. Иначе деградация в `RegimeLabel::Unclear`, plain leverage.
* **Inv-S4.** `uncertainty.level ≠ Extreme`. Иначе вход блокируется до cooldown.
* **Inv-S5.** `fingerprint_block_count_consecutive < 10`. Иначе fingerprint suppress.
* **Inv-S6.** `consecutive_rejections_ < 5`. Иначе `RiskEngine` повышает порог conviction.
* **Inv-S7.** `peak_equity − current_equity ≤ max_drawdown_pct × peak_equity`. Иначе day-lock.
* **Inv-S8.** `funding_rate * leverage ≤ funding_threshold`. Иначе reduce leverage.

### 8.4 Инварианты данных

* **Inv-D1.** Каждый `NormalizedEvent` имеет `EventEnvelope.received_ns ≥ exchange_ts_ns − clock_offset_ms·1e6`.
* **Inv-D2.** `OrderBookSnapshot` всегда первый в потоке для символа; `apply_delta` без предшествующего snapshot отвергается.
* **Inv-D3.** `Position.opened_at ≤ Position.updated_at`, оба = monotonic timestamps.

### 8.5 Инварианты конкурентности

* **Inv-C1.** Никакой объект `tb::*` не разделяется между потоками без `mutex` или `atomic`.
* **Inv-C2.** Колбэки `IClock` thread-safe.
* **Inv-C3.** `LocalOrderBook::subscribe()` вызывается только в монтаже pipeline; во время `emit_events` подписка не меняется.

---

## 9. Контрактное программирование (DbC)

Для критических модулей фиксируется минимальный набор контрактов. Каждый модульный `claude.md` уточняет контракты для конкретных классов.

### 9.1 RiskEngine

#### `IRiskEngine::evaluate(intent, sizing, portfolio, features, exec_alpha, uncertainty)`

**Pre.**
* `intent.symbol.get() != "" ∧ intent.signal_intent != Hold`.
* `sizing.suggested_quantity.get() > 0 ∧ sizing.suggested_quantity.get() ≤ sizing.max_quantity.get()`.
* `portfolio.computed_at.get() ≥ now − 1s`.
* `features.is_stale = false` (иначе вызов вообще не должен происходить).
* `exec_alpha.style != Invalid`.

**Post.**
* `decision.decided_at = now()`.
* `decision.allowed → decision.approved_quantity.get() ∈ (0, sizing.suggested_quantity.get()]`.
* `decision.allowed → ¬kill_switch_active`.
* `decision.action = Deny → ¬decision.allowed`.
* `decision.triggered_checks` непуст ⇔ некоторая проверка срабатывала.

**Invariant.**
* `record_order_sent`/`record_trade_result`/`record_trade_close` вызываются ИСКЛЮЧИТЕЛЬНО после фактического действия (не на intent), иначе нарушается consecutive losses tracking.

### 9.2 ExecutionEngine

#### `Result<OrderId> execute(intent, risk_decision, exec_alpha, uncertainty)`

**Pre.**
* `risk_decision.allowed = true ∧ risk_decision.approved_quantity.get() > 0`.
* `intent.symbol = risk_decision`-ассоциированный символ.
* Margin зарезервирован: `portfolio_->reserve_cash(...)` выполнен (внутри `try_reserve_margin`).
* `submitter_` не nullptr.

**Post.**
* Успех: возвращён `OrderId o` ∧ `registry_.get_order(o).has_value() ∧ FSM::current_state ∈ {New, PendingNew, Open, PartiallyFilled}`.
* Неудача: возвращён `Err(...)` ∧ зарезервированный cash освобождён.

**Invariant.**
* Дедупликация: при `is_duplicate(intent)` сразу `Err`.
* Мьютекс `execute_mutex_` сериализует concurrent execute.

### 9.3 OrderRegistry

#### `void register_order(OrderRecord o)`

**Pre.** `o.order_id` уникален (`get_order(o.order_id) = nullopt`).  
**Post.** `get_order(o.order_id) = o`, FSM создан в `New`, `active_count` инкрементирован.

#### `bool transition(OrderId, OrderState, reason)`

**Pre.** Запись существует.  
**Post.** Если допустим — состояние обновлено, `history` дополнена `OrderTransition`, иначе состояние не изменилось.  
**Invariant.** `force_transition` не вызывается из обычного pipeline (только из `RecoveryManager`).

### 9.4 PortfolioEngine

#### `bool reserve_cash(order_id, symbol, side, notional, fee, strategy_id)`

**Pre.** `order_id` ∉ pending_orders. `notional > 0 ∧ fee ≥ 0`.  
**Post.** `available_cash` уменьшен на `notional/leverage + fee`, `pending_orders[order_id]` создан.  
**Invariant.** `Σ reserved + Σ position_margin + available_cash = total_capital + Δpnl − fees`.

#### `void open_position(Position p)`

**Pre.** `p.size > 0 ∧ p.avg_entry_price > 0 ∧ p.symbol != ""`. Inv-2.  
**Post.** Позиция в `positions_[make_key(p.symbol, p.position_side)]`, событие `PositionOpened` записано.  
**Invariant.** В hedge-mode разрешены 2 позиции на символ (Long+Short).

### 9.5 OrderSubmitter (Bitget Futures)

#### `OrderSubmitResult submit_order(OrderRecord o)`

**Pre.** `o.symbol` имеет правила (`set_rules` вызывался). `o.qty` ≥ `min_qty`, `notional` ≥ `min_notional`. Leverage установлен (Inv-11).  
**Post.** Успех: `result.exchange_order_id != ""`. Неудача: `result.error_code` ∈ Bitget-кодов.  
**Invariant.** `tradeSide ∈ {open, close}` всегда устанавливается (Bitget rejecting otherwise).

### 9.6 LocalOrderBook

#### `bool apply_delta(NormalizedOrderBook delta)`

**Pre.** `last_sequence_ != 0` (snapshot уже применён). `delta.sequence > last_sequence_`.  
**Post.** Успех: уровни bids/asks обновлены, `last_sequence_ = delta.sequence`, `quality_ = OK`. Неудача: `quality_ = OutOfSync`, `request_resync()` инициирован, возвращён `false`.  
**Invariant.** `bids_.begin().key < asks_.begin().key` (книга не crossed).

### 9.7 IdempotencyManager

#### `string generate_client_order_id(symbol, side, strategy_id)`

**Pre.** Все аргументы непустые.  
**Post.** Возвращённый ID уникален в окне `dedup_window_ms` среди всех ранее сгенерированных.  
**Invariant.** Длина ID ≤ Bitget предела (40 символов для clientOid).

### 9.8 Supervisor

#### `bool try_lock_symbol(symbol, pipeline_id)`

**Pre.** `pipeline_id != ""`.  
**Post.** Успех: `is_symbol_locked(symbol) = true ∧ symbol_locks_[symbol] = pipeline_id`. Неудача: символ уже залочен другим pipeline.  
**Invariant.** Reentrant: один и тот же pipeline_id может «получить» лок повторно (idempotent).

---

## 10. Зависимости

### 10.1 Внешние библиотеки

| Библиотека | Версия | Где используется |
|------------|--------|-------------------|
| Boost.System | 1.82+ | error_code |
| Boost.Asio + Beast | 1.82+ | REST/WS клиенты |
| Boost.JSON | 1.82+ | парсинг |
| Boost.Thread | 1.82+ | (косвенно через Asio) |
| OpenSSL | 3.x | TLS, HMAC-SHA256 (signing) |
| libpqxx | 7.x | PostgreSQL |
| Catch2 v3 | FetchContent | тесты |

### 10.2 Внутренние зависимости (граф)

Уровень 0 (foundation): `common`, `clock`, `logging`, `metrics`, `security`, `config`.
Уровень 1: `buffers`, `resilience`, `persistence`, `exchange/bitget`.
Уровень 2: `normalizer`, `order_book`, `indicators`, `features`, `market_data` (зависит от exchange).
Уровень 3: `world_model`, `regime`, `uncertainty`.
Уровень 4: `ml`, `strategy` (+ sub), `decision` (включает `decision/strategy_allocation.hpp` после удаления `strategy_allocator/` в scalping refactor).
Уровень 5: `portfolio`, `portfolio_allocator`, `leverage`, `execution_alpha`, `opportunity_cost`, `cost_attribution`, `risk` (+ sub).
Уровень 6: `execution` (+ sub), `reconciliation`, `recovery`, `scanner`, `telemetry`.
Уровень 7: `pipeline`, `supervisor`, `app`.

Все зависимости направленные, циклов на текущей карте нет (после удаления orphaned модулей).

---

## 11. Риск-профиль архитектуры

| ID | Категория | Описание | Вероятность | Влияние |
|----|-----------|----------|-------------|---------|
| R-1 | Хост / биржа | Сетевой сбой → задержка REST → backlog WS | средняя | критическое |
| R-2 | Латентность | REST в hot-path (Defect-D2) → пропуск тиков | высокая | высокое |
| R-3 | Конкурентность | Inversion `pipeline_mutex_` ↔ `Supervisor::*` (Inv-10) | низкая | критическое |
| R-4 | Состояние | Race kill-switch ↔ `RiskEngine::evaluate` (Defect-D3) | средняя | высокое |
| R-5 | Сборка | Orphaned модули (Defect-D1) → отключённые защиты | константа | среднее |
| R-6 | Конфигурация | Самописный flat YAML парсер → молчаливое игнорирование вложенных полей | низкая | высокое |
| R-7 | Биржа | Отсутствие верификации set_leverage / set_margin_mode перед первым ордером (Defect-D4) | средняя | высокое |
| R-8 | Алгоритм | Нет отдельных тестов на симметрию hedge-mode (Long+Short одновременно) — только в integration | низкая | высокое |
| R-9 | Восстановление | `RecoveryService` не верифицирует TP/SL trigger ордера на бирже после рестарта (есть `extended_recovery`, но не во всех путях) | средняя | критическое |
| R-10 | Конфиг хеш | `ConfigHash` (SHA-256) рассчитывается до hot-reload параметров leverage_engine — расхождение при runtime change | низкая | низкое |

---

## 12. Выявленные дефекты — финальный статус

Колонка **Status**: ✅ IMPLEMENTED (правка в коде, сборка clean), ✗ NOT-A-DEFECT (false positive исходного аудита).

### Обновление 2026-05-08 — Production hardening pass 2

После первой волны рефакторинга проведена дополнительная итерация:

* **Pre-existing failing tests (8 тестов) — все исправлены:**
  - `FileSecretProvider`: тесты не выставляли `chmod 0600` на test-fixture .env. Production требует strict permissions; тесты исправлены — добавлен `std::filesystem::permissions(..., owner_read|owner_write)`.
  - `IdempotencyManager`: внутренний `now_ms()` использовал `std::chrono::steady_clock::now()` напрямую, **полностью игнорируя injected `IClock`**. Это нарушало testability (TestClock не работал) и concealed potential bugs. Исправлено: `now_ms()` теперь использует `clock_->now()` если provided.
  - `HedgePairManager`: тесты ожидали immediate transition в `PrimaryPlusHedge` после `notify_hedge_opened()`, но production использует two-phase commit (через `input.has_hedge=true` в evaluate). Добавлен explicit метод `confirm_hedge_filled()` для явного подтверждения; тесты обновлены.
* **Новые runtime-проверки риска (`RiskEngine`):**
  - `ReconciliationDesyncCheck` (#36) — блокирует НОВЫЕ entry-ордера, когда `reconciliation_engine_` зафиксировала mismatch с биржей. Close/Reduce разрешены (для сокращения exposure).
  - `WsDisconnectedCheck` (#37) — блокирует все entry/exit когда public WebSocket отвалился. `TradingPipeline::run_periodic_tasks` пропагирует состояние через `risk_engine_->set_ws_disconnected(!is_connected())`.
* **K8s probes:**
  - Новый класс [`HealthState`](src/app/health_state.hpp) — атомарное состояние liveness/readiness.
  - `/livez` (200/503), `/readyz` (200/503 c reason), `/healthz` (alias `/readyz`) подключены к существующему `HttpEndpointServer`.
  - `g_health_state` обновляется из signal handler (shutdown_requested), supervisor.start/stop, и фонового `health_monitor_thread` (jthread, 1Hz polling) который читает `connected_pipeline_count`/`is_kill_switch_active`/`is_degraded`.
  - `is_ready()` ⇔ `subsystems_started ∧ ≥1 connected pipeline ∧ ¬kill_switch ∧ ¬degraded ∧ ¬shutdown_requested`.



| ID | Severity | Status | Файл / место | Что сделано |
|----|----------|--------|--------------|------------|
| D1 | HIGH | ✅ IMPLEMENTED | `src/{adversarial_defense,alpha_decay,drift,self_diagnosis,validation}` | 5 orphan-модулей и их orphan-тесты удалены целиком. Adversarial detection живёт inline в pipeline (`trading_pipeline.cpp:3849-3921`). |
| D2 | CRITICAL | ✅ IMPLEMENTED | `pipeline/rest_worker_pool.{hpp,cpp}` (новый), `trading_pipeline.cpp::run_periodic_tasks` | Создан `RestWorkerPool` (jthread + bounded MPSC + drop-oldest). Все 3 периодические REST-задачи (`balance_sync`, `reconciliation`, `funding_rate`) переведены на async submit. Atomic in-flight маркеры. Drain on stop(). |
| D3 | HIGH | ✅ IMPLEMENTED | `risk/risk_engine.{hpp,cpp}`, `pipeline/trading_pipeline.{hpp,cpp}`, `app/main.cpp` | Добавлен `KillSwitchProvider` callback в `IRiskEngine`. На каждом `evaluate()` риск-движок синхронизирует local state из authoritative источника (`Supervisor::is_kill_switch_active()`). Race window закрыт. |
| D4 | HIGH | ✅ IMPLEMENTED | `pipeline/trading_pipeline.cpp::start` | Все 4 проверки (`set_hold_mode`, `set_margin_mode`, `set_leverage long`, `set_leverage short`) теперь fail-fast: при отказе → `start()` returns false → Supervisor не запускает pipeline. |
| D5 | MEDIUM | ✅ IMPLEMENTED | `config/CMakeLists.txt`, `config/config_loader.cpp` | Самописный flat-парсер заменён на `yaml-cpp 0.8`. Используется `YAML::Load` + рекурсивный flatten. Sequences сериализуются в comma-separated string (совместимо с downstream `parse_list`). Sentinel `__yaml_parse_error` для fail-fast при битом YAML. |
| D6 | HIGH | ✅ IMPLEMENTED | `pipeline/{trading_history_stats,funding_rate_tracker,htf_trend_state}.hpp` (новые) | Извлечены 3 класса. `TradingHistoryStats` — rolling window + Kelly stats. `FundingRateTracker` — атомарный funding rate + интервалы. `HtfTrendState` — все 16 полей старшего таймфрейма в одном агрегаторе. Pipeline хранит как member-объекты. |
| D7 | MEDIUM | ✗ NOT-A-DEFECT | `order_book/order_book.cpp::emit_events` | После ревизии: уже корректно (BUG-ML-01 fix at lines 352-361). |
| D8 | MEDIUM | ✅ IMPLEMENTED | `app/main.cpp` | `interruptible_sleep` + early signal handler (200мс гранулярность). SIGTERM/SIGINT во время scanner-retry → корректный exit. |
| D9 | MEDIUM | ✗ NOT-A-DEFECT | `execution/execution_engine_new.cpp::execute` | После ревизии: `try_reserve_margin` вызывается **после** `is_duplicate` под `execute_mutex_`. Try/catch на submit с `release_cash`. Ghost-reservation невозможен. |
| D10 | LOW | 📝 DOCUMENTED | `clock/claude.md` | Wall vs monotonic задокументировано. |
| D11 | LOW | ✗ NOT-A-DEFECT | `exchange/bitget/bitget_rest_client.cpp::wait_for_rate_limit` | После ревизии: `lock.unlock()` перед `sleep_for`, `lock.lock()` после (lines 145-147). Корректно. |
| D12 | LOW | ✅ IMPLEMENTED | `app/main.cpp` ↔ `scanner/scanner_config.hpp` | Hardcoded thresholds вынесены в `ScannerConfig::micro_account_*`. |
| D13 | INFO | ✅ IMPLEMENTED | `audit.md`, `plan.md`, `.gitignore` | Удалены + в `.gitignore`. |
| D14 | INFO | ✅ IMPLEMENTED | `.gitignore` | `Testing/`, CTest artefacts добавлены. |
| **+** | bonus | ✅ IMPLEMENTED | `src/resilience/CMakeLists.txt` | Pre-existing CMake bug — `tb_resilience` не публиковала `Boost::json`/`Boost::system` в PUBLIC link interface. `retry_executor.cpp` использует `boost::json` для классификации ошибок → `test_resilience` не линковалось. Исправлено — теперь link clean. |

### Сводный риск-профиль (после рефакторинга)

| Риск | Состояние |
|------|-----------|
| Orphan-код в src/ | ✅ Устранён |
| REST блокирует hot-path (CRITICAL) | ✅ Устранён (3/3 задачи async через `RestWorkerPool`) |
| Kill-switch race (HIGH) | ✅ Устранён (provider sync в evaluate) |
| Mismatched leverage без alert (HIGH) | ✅ Устранён (fail-fast) |
| Хрупкий YAML парсер (MEDIUM) | ✅ Устранён (yaml-cpp 0.8) |
| God-object TradingPipeline (HIGH) | ✅ Извлечены 3 класса; продолжающаяся декомпозиция в [`docs/refactor_plan.md`](docs/refactor_plan.md) |
| TSAN coverage | ⏳ Open (требует CI infrastructure) |

---

## 13. Рекомендации (приоритезированные)

### 13.0 Закрыто в рефакторинге 2026-05-07

* ✅ R-1 (D1) — orphans удалены, ноль dead-code в `src/`.
* ✅ R-Small1 (D8) — `interruptible_sleep` в `app/main.cpp`.
* ✅ R-Small2 (D12) — micro-account scaling в `ScannerConfig`.
* ✅ R-Small3 (D13, D14) — пустые placeholder-файлы и `Testing/` artefacts.

### 13.1 Открытые критические (см. `docs/refactor_plan.md`)

1. **R-2 (D2).** Вынести все REST-вызовы из hot-path в фоновые worker-потоки с MPSC-очередью результатов. Применить шаблон `update_reference_prices` ко всем периодическим задачам. **Обязательная предусловная задача:** TSAN-сборка в CI.
2. **R-3 (D3).** Унифицировать kill-switch через provider-callback: `IRiskEngine::set_kill_switch_provider(...)`. Удалить локальный `kill_switch_active_` из `ProductionRiskEngine`.
3. **R-4 (D4).** Fail-fast верификация `set_leverage` / `set_margin_mode` / `set_hold_mode` в `TradingPipeline::start`. При отказе — `Supervisor::mark_symbol_unsafe(symbol)` + блокировка.
4. **R-5 (D5).** Замена self-baked YAML-парсера на `yaml-cpp` со strict-mode (unknown keys → fail). Snapshot-тест на существующий `production.yaml`.

### 13.2 Высокие

5. **R-6 + R-7 (D6).** Декомпозировать `TradingPipeline`: вынести `HtfTrendTracker`, `TradingHistoryStats`, `FundingRateTracker`, `TrailingStopManager`, `SnapshotPersister`, `DiagCounters` в отдельные классы. Split mutex `market_state_mutex_` ↔ `execution_state_mutex_`. **Обязательно после R-2 + TSAN.**
6. **R-8.** Включить TSAN в CI (один из конфигов сборки). Проверить Inv-C1 ÷ Inv-C3. **Блокер для R-2 и R-6.**
7. **R-9.** Добавить runtime-проверку Inv-5 (cash conservation) в каждый pipeline tick, при нарушении — kill-switch + alert. Сейчас только `check_invariants()` (необязательный метод).
8. **R-10.** Расширить `Reconciliation` на trigger-orders (TP/SL). Уже есть `get_trigger_orders` в интерфейсе, но default возвращает пустой список — реализовать в Bitget-адаптере.

### 13.3 Средние

9. **R-11.** Заменить `std::map<Price,Quantity>` в `LocalOrderBook` на отсортированный плоский вектор/`boost::container::flat_map` для cache-friendly обновлений.
10. **R-12.** Инкрементальные индикаторы: `IndicatorEngine` сейчас принимает полный вектор — переделать на streaming-обновления.
11. **R-13.** SDK для тестирования: моки `IExchangeQueryService`, `IOrderSubmitter` уже упомянуты как факт; задокументировать публичный набор моков в `tests/common/`.
12. **R-14.** Добавить ctest-таргет `arch-tests` (Catch2): инвариантные проверки § 8 на смоделированных трейс-данных.

### 13.4 Низкие

13. **R-17.** Добавить `/healthz` и `/readyz` к `HttpEndpointServer`, чтобы Kubernetes-готовый.
14. **R-18.** Pool-allocator для `boost::json::value` в `Normalizer` (если профайлинг подтвердит hot-spot).
15. **R-19.** Расширить `IClock` методом `monotonic_ns()` (D10 documentation note → API change).

---

## 14. Стандарт проверки (как читать этот документ)

* Контракты в § 9 и инварианты в § 8 — обязательны при ревью PR.
* Перед production-релизом запустить:
  - Catch2 (556 тестов), все зелёные;
  - TSAN-сборку с прогоном integration-тестов (после R-8);
  - `ProductionGuard` validate (Inv-1);
  - Reconciliation против биржи (Inv-12) — startup baseline;
  - Smoke-tест staged_rollout (см. `configs/staged_rollout.yaml`).
* Любая модификация `risk/policies/*` без обновления § 8 / § 9 — automatic block.

---

*Документ создан на основе статического анализа исходного кода без обращения к внешним системам и без запуска бинарника. Возможны точки, которые требуют дополнительной верификации (помечены «требует верификации» или явным указанием Defect/Risk).*
