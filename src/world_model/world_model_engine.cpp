#include "world_model/world_model_engine.hpp"
#include <algorithm>
#include <cmath>

namespace tb::world_model {

// ─── Defaults preserved for config loader / validator compatibility ──────────

SuitabilityConfig SuitabilityConfig::make_default() {
    // The 9×N state×strategy table was retired with the adaptive engine.
    // Single-strategy bot relies on regime hints + uncertainty for gating.
    return SuitabilityConfig{};
}

WorldModelConfig WorldModelConfig::make_default() {
    WorldModelConfig cfg;
    cfg.suitability = SuitabilityConfig::make_default();
    return cfg;
}

// ─── String helpers (kept for telemetry/log compatibility) ───────────────────

std::string to_string(WorldState state) {
    switch (state) {
        case WorldState::StableTrendContinuation:    return "StableTrendContinuation";
        case WorldState::FragileBreakout:            return "FragileBreakout";
        case WorldState::CompressionBeforeExpansion: return "CompressionBeforeExpansion";
        case WorldState::ChopNoise:                  return "ChopNoise";
        case WorldState::ExhaustionSpike:            return "ExhaustionSpike";
        case WorldState::LiquidityVacuum:            return "LiquidityVacuum";
        case WorldState::ToxicMicrostructure:        return "ToxicMicrostructure";
        case WorldState::PostShockStabilization:     return "PostShockStabilization";
        case WorldState::Unknown:                    return "Unknown";
    }
    return "Unknown";
}

std::string to_string(TransitionTendency tendency) {
    switch (tendency) {
        case TransitionTendency::Stable:        return "Stable";
        case TransitionTendency::Improving:     return "Improving";
        case TransitionTendency::Deteriorating: return "Deteriorating";
        case TransitionTendency::Ambiguous:     return "Ambiguous";
    }
    return "Ambiguous";
}

WorldStateLabel WorldModelSnapshot::to_label(WorldState s) {
    switch (s) {
        case WorldState::StableTrendContinuation:
            return WorldStateLabel::Stable;
        case WorldState::FragileBreakout:
        case WorldState::CompressionBeforeExpansion:
        case WorldState::PostShockStabilization:
        case WorldState::ChopNoise:
            return WorldStateLabel::Transitioning;
        case WorldState::ExhaustionSpike:
        case WorldState::LiquidityVacuum:
        case WorldState::ToxicMicrostructure:
            return WorldStateLabel::Disrupted;
        case WorldState::Unknown:
            return WorldStateLabel::Unknown;
    }
    return WorldStateLabel::Unknown;
}

// ─── Engine ─────────────────────────────────────────────────────────────────

RuleBasedWorldModelEngine::RuleBasedWorldModelEngine(
    WorldModelConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock)) {}

RuleBasedWorldModelEngine::RuleBasedWorldModelEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(WorldModelConfig::make_default())
    , logger_(std::move(logger))
    , clock_(std::move(clock)) {}

namespace {

// Minimal classifier: spread/VPIN/imbalance/ADX → one of 5 useful states.
// The remaining 4 states from the legacy 9-state enum are never produced.
WorldState classify(const features::FeatureSnapshot& snap, const WorldModelConfig& cfg) {
    const auto& m = snap.microstructure;
    const auto& t = snap.technical;

    // Disrupted: toxic micro / vacuum first — these are hard "don't trade" signals.
    if (m.spread_valid && m.spread_bps >= cfg.liquidity_vacuum.spread_bps_critical) {
        return WorldState::LiquidityVacuum;
    }
    if (m.vpin_valid && m.vpin_toxic) {
        return WorldState::ToxicMicrostructure;
    }
    if (m.spread_valid && m.book_instability >= cfg.toxic.book_instability_min &&
        m.spread_bps >= cfg.toxic.spread_bps_min) {
        return WorldState::ToxicMicrostructure;
    }
    if (m.liquidity_valid && m.liquidity_ratio < cfg.liquidity_vacuum.liquidity_ratio_min) {
        return WorldState::LiquidityVacuum;
    }

    // Trending: ADX strong + directional momentum.
    // B18.2: 5 bps directional momentum threshold (можно в WorldModelConfig).
    constexpr double kTrendingMomentumThreshold = 0.0005;
    if (t.adx_valid && t.adx >= cfg.stable_trend.adx_min && t.momentum_valid) {
        if (std::abs(t.momentum_20) > kTrendingMomentumThreshold) {
            return WorldState::StableTrendContinuation;
        }
    }

    // Chop: low ADX, low momentum.
    if (t.adx_valid && t.adx <= cfg.chop_noise.adx_max) {
        return WorldState::ChopNoise;
    }

    // Exhaustion: extreme RSI with reversal momentum.
    if (t.rsi_valid && t.momentum_valid) {
        if (t.rsi_14 >= cfg.exhaustion.rsi_upper && t.momentum_5 < 0.0) {
            return WorldState::ExhaustionSpike;
        }
        if (t.rsi_14 <= cfg.exhaustion.rsi_lower && t.momentum_5 > 0.0) {
            return WorldState::ExhaustionSpike;
        }
    }

    return WorldState::Unknown;
}

double simple_fragility(const features::FeatureSnapshot& snap) {
    // B18.3: эмпирические фрагменты-веса. Можно вынести в WorldModelConfig
    // если потребуется per-symbol калибровка.
    constexpr double kBaseFragility       = 0.3;
    constexpr double kSpreadFragSeverity  = 0.3;
    constexpr double kSpreadBpsThreshold  = 20.0;
    constexpr double kInstabFragSeverity  = 0.2;
    constexpr double kInstabThreshold     = 0.5;
    constexpr double kVpinFragSeverity    = 0.2;
    const auto& m = snap.microstructure;
    double frag = kBaseFragility;
    if (m.spread_valid && m.spread_bps > kSpreadBpsThreshold) frag += kSpreadFragSeverity;
    if (m.book_instability > kInstabThreshold)                frag += kInstabFragSeverity;
    if (m.vpin_valid && m.vpin_toxic)                         frag += kVpinFragSeverity;
    return std::clamp(frag, 0.0, 1.0);
}

StateProbabilities make_probabilities(WorldState primary) {
    StateProbabilities p;
    p.valid = true;
    // 0.7 mass on the chosen state, the rest spread evenly across the
    // four "bad" states that the decision engine penalises. This keeps
    // downstream consumers (decision/uncertainty) functioning while
    // removing the full transition-matrix machinery.
    constexpr double kPrimary = 0.7;
    constexpr double kBadSlice = 0.075;
    for (size_t i = 0; i < kWorldStateCount; ++i) p.values[i] = 0.0;
    p.values[static_cast<int>(primary)] = kPrimary;

    const std::array<WorldState, 4> bad = {
        WorldState::ToxicMicrostructure,
        WorldState::LiquidityVacuum,
        WorldState::ExhaustionSpike,
        WorldState::ChopNoise
    };
    for (auto s : bad) {
        if (s != primary) p.values[static_cast<int>(s)] += kBadSlice;
    }
    return p;
}

} // namespace

WorldModelSnapshot RuleBasedWorldModelEngine::update(const features::FeatureSnapshot& snap) {
    std::lock_guard lk(mutex_);

    WorldModelSnapshot out;
    out.symbol = snap.symbol;
    out.computed_at = clock_ ? clock_->now() : Timestamp(0);

    out.state = classify(snap, config_);
    out.label = WorldModelSnapshot::to_label(out.state);

    out.fragility.value = simple_fragility(snap);
    out.fragility.valid = true;
    out.fragility.confidence = 0.7;

    out.persistence_score = 0.5;
    out.tendency = TransitionTendency::Stable;
    out.confidence = 0.7;
    out.state_probabilities = make_probabilities(out.state);
    out.model_version = "3.0.0-min";
    out.dwell_ticks = 0;

    // Single-strategy bot: emit one neutral suitability entry for the
    // active scalp engine. Pipeline multiplies allocator weight by it.
    StrategySuitability s;
    s.strategy_id = StrategyId("scalp_engine");
    s.suitability = (out.label == WorldStateLabel::Disrupted) ? 0.3 : 1.0;
    s.execution_suitability = 1.0;
    s.risk_suitability = 1.0;
    s.signal_suitability = s.suitability;
    s.vetoed = false;
    out.strategy_suitability.push_back(std::move(s));

    last_[snap.symbol.get()] = out;
    return out;
}

std::optional<WorldModelSnapshot> RuleBasedWorldModelEngine::current_state(
    const Symbol& symbol) const {
    std::lock_guard lk(mutex_);
    auto it = last_.find(symbol.get());
    if (it == last_.end()) return std::nullopt;
    return it->second;
}

} // namespace tb::world_model
