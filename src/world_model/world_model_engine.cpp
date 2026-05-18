#include "world_model/world_model_engine.hpp"
#include <algorithm>
#include <cmath>

namespace tb::world_model {

WorldModelConfig WorldModelConfig::make_default() {
    return WorldModelConfig{};
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

    if (t.adx_valid && t.adx >= cfg.stable_trend.adx_min && t.momentum_valid) {
        if (std::abs(t.momentum_20) > cfg.stable_trend.trending_momentum_threshold) {
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

double simple_fragility(const features::FeatureSnapshot& snap, const WorldModelConfig& cfg) {
    const auto& m = snap.microstructure;
    const auto& f = cfg.fragility;
    double frag = f.base;
    if (m.spread_valid && m.spread_bps > f.spread_bps_threshold) frag += f.spread_severity;
    if (m.book_instability > f.instab_threshold)                  frag += f.instab_severity;
    if (m.vpin_valid && m.vpin_toxic)                             frag += f.vpin_severity;
    return std::clamp(frag, 0.0, 1.0);
}

StateProbabilities make_probabilities(WorldState primary) {
    StateProbabilities p;
    p.valid = true;
    constexpr double kPrimary = 0.7;
    for (size_t i = 0; i < kWorldStateCount; ++i) p.values[i] = 0.0;

    const std::array<WorldState, 4> bad = {
        WorldState::ToxicMicrostructure,
        WorldState::LiquidityVacuum,
        WorldState::ExhaustionSpike,
        WorldState::ChopNoise
    };

    size_t bad_slots = 0;
    for (auto s : bad) if (s != primary) ++bad_slots;

    p.values[static_cast<int>(primary)] = kPrimary;
    const double slice = bad_slots > 0 ? (1.0 - kPrimary) / static_cast<double>(bad_slots) : 0.0;
    for (auto s : bad) {
        if (s != primary) p.values[static_cast<int>(s)] = slice;
    }

    double sum = 0.0;
    for (size_t i = 0; i < kWorldStateCount; ++i) sum += p.values[i];
    if (sum > 0.0 && std::abs(sum - 1.0) > 1e-9) {
        for (size_t i = 0; i < kWorldStateCount; ++i) p.values[i] /= sum;
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

    out.fragility.value = simple_fragility(snap, config_);
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
