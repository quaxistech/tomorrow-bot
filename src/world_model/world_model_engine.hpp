#pragma once
#include "world_model/world_model_types.hpp"
#include "world_model/world_model_config.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace tb::world_model {

/// Minimal world-model engine for Bitget USDT-M scalping.
///
/// The legacy 9-state adaptive machine with hysteresis, transition matrices,
/// feedback EMAs and probabilistic ensembles was removed in the 2026-05
/// scalping refactor. The bot trades a single strategy on a small account;
/// the only downstream needs are:
///   * a label suitable for telemetry/decision context,
///   * suitability=1.0 for the active strategy (degenerate at N=1),
///   * a rough state_probabilities slice over the four "bad" states the
///     decision engine penalises (Toxic, Vacuum, Exhaustion, Chop).
///
/// All other snapshot fields are populated to safe defaults.
class IWorldModelEngine {
public:
    virtual ~IWorldModelEngine() = default;

    virtual WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) = 0;
    virtual std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const = 0;

    virtual void record_feedback(const WorldStateFeedback& feedback) { (void)feedback; }
    virtual std::string model_version() const { return "3.0.0-min"; }
};

class RuleBasedWorldModelEngine : public IWorldModelEngine {
public:
    RuleBasedWorldModelEngine(WorldModelConfig config,
                              std::shared_ptr<logging::ILogger> logger,
                              std::shared_ptr<clock::IClock> clock);

    RuleBasedWorldModelEngine(std::shared_ptr<logging::ILogger> logger,
                              std::shared_ptr<clock::IClock> clock);

    WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) override;
    std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const override;
    std::string model_version() const override { return "3.0.0-min"; }

private:
    WorldModelConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorldModelSnapshot> last_;
};

} // namespace tb::world_model
