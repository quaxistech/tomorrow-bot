#pragma once

#include "uncertainty/uncertainty_types.hpp"
#include "features/feature_snapshot.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward declarations
namespace tb::portfolio { struct PortfolioSnapshot; }
namespace tb::ml { struct MlSignalSnapshot; }

namespace tb::uncertainty {

/// Lightweight uncertainty gate for Bitget USDT-M scalping.
///
/// Replaces the legacy 9-dimensional engine (regime/signal/data/exec/portfolio/
/// ml/correlation/transition/operational) with a 4-signal aggregator focused
/// on the failure modes a $15-account scalper actually suffers from:
/// wide spread, stale feed, toxic VPIN, book instability. See engine.cpp for
/// the scoring rule. Types preserved for downstream compatibility.
class IUncertaintyEngine {
public:
    virtual ~IUncertaintyEngine() = default;

    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) = 0;

    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) = 0;

    virtual UncertaintyDiagnostics diagnostics() const = 0;
    virtual void reset_state() = 0;
};

/// Per-symbol state kept across ticks for EMA smoothing + cooldowns.
struct SymbolState {
    double ema_score{0.5};
    UncertaintyLevel prev_level{UncertaintyLevel::Moderate};
    int64_t last_assess_ns{0};
    int64_t cooldown_until_ns{0};
    int consecutive_extreme{0};
    int consecutive_high{0};
};

class RuleBasedUncertaintyEngine final : public IUncertaintyEngine {
public:
    RuleBasedUncertaintyEngine(UncertaintyConfig config,
                                std::shared_ptr<logging::ILogger> logger,
                                std::shared_ptr<clock::IClock> clock);

    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) override;

    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) override;

    UncertaintyDiagnostics diagnostics() const override;
    void reset_state() override;

private:
    UncertaintyConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    std::unordered_map<std::string, UncertaintySnapshot> snapshots_;
    std::unordered_map<std::string, SymbolState> states_;
    UncertaintyDiagnostics diagnostics_;

    mutable std::mutex mutex_;
};

} // namespace tb::uncertainty
