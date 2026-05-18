# Pre-trade Gates (edge-31 Phase 2)

**Файлы:** `pre_trade_gates.{hpp,cpp}`
**Введено:** Phase 2 TPSL refactor (2026-05-16)

## Назначение

Два независимых gates выполняются между risk_engine evaluation и execution_engine.execute().
Защищают entry от случаев, где risk_engine OK но факт сделки toxic:

1. **FreshnessGate** — сигнал устарел или контекст изменился между build_intent и execute.
2. **NetRRGate** — gross R:R looks fine но после fees+slippage+funding net R:R ниже порога.

## API

```cpp
class FreshnessGate {
  FreshnessGate(const PreTradeGatesConfig& cfg);
  FreshnessVerdict evaluate(const TradeIntent& intent,
                              const FeatureSnapshot& current_snapshot,
                              int64_t now_ns);
};

class NetRRGate {
  NetRRGate(const PreTradeGatesConfig& cfg);
  NetRRVerdict evaluate(const TradeIntent& intent,
                         double current_mid,
                         double funding_rate_8h,
                         bool is_taker);
};
```

## Verdict structs

```cpp
struct FreshnessVerdict {
  bool passed;
  std::string reason_code;        // "ok", "stale", "price_drift", "spread_widened", "depth_thin"
  std::string reason_detail;
  int64_t signal_age_ms;
  double price_drift_bps;         // signed: positive = adverse
  double spread_widen_pct;
  double depth_remain_pct;
};

struct NetRRVerdict {
  bool passed;
  std::string reason_code;        // "ok", "no_tp_sl", "insufficient_net_rr", "invalid_tp_sl"
  double gross_reward_bps;
  double gross_risk_bps;
  double gross_rr;
  double total_cost_bps;          // fees + slippage [+ funding]
  double net_reward_bps;
  double net_rr;
};
```

## Config (PreTradeGatesConfig в `config_types.hpp`)

```yaml
pre_trade_gates:
  freshness_enabled: true
  max_signal_age_ms: 1500          # signal → execute window
  max_adverse_price_drift_bps: 8.0
  max_spread_widen_pct: 50.0
  min_depth_remain_pct: 50.0
  net_rr_enabled: true
  min_net_rr: 0.5                  # min ((tp_bps - cost) / sl_bps)
  assumed_slippage_bps_per_leg: 10.0  # run95: 2 → 10 (observed avg 46 bps)
  taker_fee_bps: 6.0
  maker_fee_bps: 2.0
  include_funding_cost: false
  assumed_hold_minutes: 2.0
```

## Поведение

- **Close intents** (trade_side=Close) bypass обоих gates — закрытие должно исполниться безусловно.
- **Open intents без snapshot data** (legacy без signal_snapshot_*) pass freshness (no-op).
- **Adverse price drift** считается signed: BUY → drift positive если цена выросла (entry дороже). SHORT → drift positive если цена упала.
- **Net RR** учитывает round-trip fees + 2× slippage per leg. Funding опционально.

## Интеграция в trading_pipeline.cpp

```cpp
// После risk_engine.evaluate, перед execution_engine.execute:
if (intent.trade_side == TradeSide::Open) {
  auto fresh = freshness_gate_->evaluate(intent, snapshot, now_ns);
  if (!fresh.passed) {
    log + diag counter; return;  // skip execute
  }
  bool is_taker = (exec_alpha.recommended_style != ExecutionStyle::PostOnly);
  auto net_rr = net_rr_gate_->evaluate(intent, current_mid,
                                          funding_tracker_.current_rate(), is_taker);
  if (!net_rr.passed) {
    log + diag counter; return;
  }
}
```

## Тесты

`tests/unit/pipeline/pre_trade_gates_test.cpp` — 18 test cases (build/SL/TP edge cases, close skip, disabled flag, funding cost).
