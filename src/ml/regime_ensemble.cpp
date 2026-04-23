/// @file regime_ensemble.cpp
/// @brief Regime-specific ensemble implementation

#include "ml/regime_ensemble.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::ml {

RegimeEnsemble::RegimeEnsemble(RegimeEnsembleConfig config)
    : config_(std::move(config))
{}

EnsembleResult RegimeEnsemble::compute_weights(
    regime::DetailedRegime regime,
    const std::vector<StrategyId>& strategy_ids,
    Timestamp now) const {

    std::lock_guard<std::mutex> lock(mutex_);

    EnsembleResult result;
    result.regime = regime;
    result.computed_at = now;

    if (strategy_ids.empty()) return result;

    const double uniform = 1.0 / static_cast<double>(strategy_ids.size());
    double total_raw = 0.0;

    for (const auto& sid : strategy_ids) {
        Key key{sid.get(), regime};
        auto it = performance_.find(key);

        EnsembleWeight w;
        w.strategy_id = sid;

        if (it == performance_.end() || it->second.trade_count < config_.min_trades_for_confidence) {
            // No data or insufficient data: shrink to uniform prior
            w.weight = uniform;
            w.confidence = 0.0;
            w.expected_win_rate = 0.5;
            w.reason = it == performance_.end()
                ? "No data for this regime"
                : "Insufficient trades (" + std::to_string(it->second.trade_count) + ")";
        } else {
            const auto& perf = it->second;
            result.has_data = true;

            // Expected win rate from Beta posterior: E[p] = alpha / (alpha + beta)
            w.expected_win_rate = perf.alpha / (perf.alpha + perf.beta);

            // Confidence based on total observations vs shrinkage strength
            const double obs = perf.alpha + perf.beta - 2.0; // subtract prior
            w.confidence = std::min(1.0, obs / (obs + config_.shrinkage_strength));

            // Raw weight: blend between posterior mean and uniform prior
            // As confidence → 1: use posterior mean; as confidence → 0: use uniform
            double posterior_weight = w.expected_win_rate;

            // PnL bonus: slight tilt toward strategies with positive cumulative PnL
            double pnl_bonus = std::tanh(perf.cumulative_pnl_bps * config_.pnl_bonus_scale);
            posterior_weight += pnl_bonus * 0.1;
            posterior_weight = std::clamp(posterior_weight, 0.0, 1.0);

            w.weight = w.confidence * posterior_weight + (1.0 - w.confidence) * uniform;
            w.reason = "Bayesian posterior (trades=" + std::to_string(perf.trade_count)
                     + ", wr=" + std::to_string(w.expected_win_rate).substr(0, 4) + ")";
        }

        total_raw += w.weight;
        result.weights.push_back(std::move(w));
    }

    // Normalize and apply floor
    if (total_raw > 0.0) {
        for (auto& w : result.weights) {
            w.weight /= total_raw;
        }
    }

    // Apply minimum weight floor to prevent complete starvation
    const double min_w = config_.min_weight;
    bool needs_renorm = false;
    for (auto& w : result.weights) {
        if (w.weight < min_w) {
            w.weight = min_w;
            needs_renorm = true;
        }
    }
    if (needs_renorm) {
        double sum = 0.0;
        for (const auto& w : result.weights) sum += w.weight;
        if (sum > 0.0) {
            for (auto& w : result.weights) w.weight /= sum;
        }
    }

    return result;
}

void RegimeEnsemble::record_outcome(const EnsembleOutcome& outcome) {
    std::lock_guard<std::mutex> lock(mutex_);

    Key key{outcome.strategy_id.get(), outcome.regime};
    auto& perf = performance_[key];

    // Exponential decay for non-stationarity
    perf.trade_count++;
    if (perf.trade_count % config_.decay_interval == 0) {
        apply_decay(perf);
    }

    // Update Beta posterior
    if (outcome.was_profitable) {
        perf.alpha += 1.0;
    } else {
        perf.beta += 1.0;
    }

    // EMA updates
    const double label = outcome.was_profitable ? 1.0 : 0.0;
    perf.ema_win_rate = perf.ema_win_rate * (1.0 - config_.ema_alpha)
                      + label * config_.ema_alpha;
    perf.ema_pnl = perf.ema_pnl * (1.0 - config_.ema_alpha)
                 + outcome.pnl_bps * config_.ema_alpha;

    perf.cumulative_pnl_bps += outcome.pnl_bps;
    perf.last_updated = outcome.occurred_at;

    total_outcomes_++;
}

const RegimePerformance* RegimeEnsemble::get_performance(
    const StrategyId& strategy_id,
    regime::DetailedRegime regime) const {

    std::lock_guard<std::mutex> lock(mutex_);
    Key key{strategy_id.get(), regime};
    auto it = performance_.find(key);
    return it != performance_.end() ? &it->second : nullptr;
}

size_t RegimeEnsemble::total_outcomes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_outcomes_;
}

void RegimeEnsemble::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    performance_.clear();
    total_outcomes_ = 0;
}

void RegimeEnsemble::apply_decay(RegimePerformance& perf) const {
    // Shrink alpha/beta toward prior (1,1) with decay_factor
    // This implements "forgetful" Bayesian updating for non-stationarity
    perf.alpha = 1.0 + (perf.alpha - 1.0) * config_.decay_factor;
    perf.beta = 1.0 + (perf.beta - 1.0) * config_.decay_factor;
}

} // namespace tb::ml
