#pragma once
/// @file regime_ensemble.hpp
/// @brief Regime-specific ensemble weighting for strategy selection
///
/// Tracks per-strategy performance within each market regime and provides
/// calibrated ensemble weights. Uses exponential decay for non-stationarity
/// and Bayesian shrinkage toward prior weights when data is sparse.

#include "common/types.hpp"
#include "regime/regime_types.hpp"
#include "ml/calibration.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <boost/container_hash/hash.hpp>

namespace tb::ml {

/// Performance record for a single strategy in one regime
struct RegimePerformance {
    double alpha{1.0};              ///< Bayesian success count (Beta prior = 1)
    double beta{1.0};               ///< Bayesian failure count (Beta prior = 1)
    double cumulative_pnl_bps{0.0}; ///< Cumulative PnL in basis points
    double ema_win_rate{0.5};       ///< EMA of win rate
    double ema_pnl{0.0};           ///< EMA of PnL per trade
    size_t trade_count{0};          ///< Total trades in this regime
    Timestamp last_updated{Timestamp(0)};
};

/// Outcome of a trade for ensemble learning
struct EnsembleOutcome {
    StrategyId strategy_id{StrategyId("")};
    regime::DetailedRegime regime{regime::DetailedRegime::Undefined};
    double pnl_bps{0.0};           ///< Realized PnL in basis points
    bool was_profitable{false};
    Timestamp occurred_at{Timestamp(0)};
};

/// Ensemble weight for one strategy
struct EnsembleWeight {
    StrategyId strategy_id{StrategyId("")};
    double weight{0.0};             ///< Final weight [0, 1], normalized across strategies
    double confidence{0.0};         ///< Confidence in the weight estimate [0, 1]
    double expected_win_rate{0.5};  ///< Estimated win rate in current regime
    std::string reason;
};

/// Full ensemble allocation result
struct EnsembleResult {
    regime::DetailedRegime regime;
    std::vector<EnsembleWeight> weights;
    bool has_data{false};            ///< true if any performance data exists for this regime
    Timestamp computed_at{Timestamp(0)};
};

/// Configuration for the regime ensemble
struct RegimeEnsembleConfig {
    double ema_alpha{0.05};          ///< EMA smoothing for win rate/PnL
    double decay_factor{0.997};      ///< Exponential decay for alpha/beta (non-stationarity)
    size_t decay_interval{20};       ///< Apply decay every N trades
    size_t min_trades_for_confidence{10}; ///< Minimum trades before trusting regime data
    double shrinkage_strength{5.0};  ///< Bayesian shrinkage toward uniform prior
    double min_weight{0.05};         ///< Floor weight to avoid complete starvation
    double pnl_bonus_scale{0.001};   ///< How much cumulative PnL affects weight
};

/// Regime-specific ensemble that adapts strategy weights based on
/// per-regime performance history.
///
/// Theory: regime-conditioned bandit with Beta-Bernoulli arms per strategy.
/// Each (strategy, regime) pair independently tracks success/failure with
/// exponential decay for non-stationarity (Garivier & Moulines 2011).
/// When data is sparse, weights shrink toward uniform via Bayesian prior.
class RegimeEnsemble {
public:
    explicit RegimeEnsemble(RegimeEnsembleConfig config = {});

    /// Compute ensemble weights for the given regime and strategy set
    EnsembleResult compute_weights(
        regime::DetailedRegime regime,
        const std::vector<StrategyId>& strategy_ids,
        Timestamp now) const;

    /// Record a trade outcome for online learning
    void record_outcome(const EnsembleOutcome& outcome);

    /// Get raw performance data for inspection/logging
    const RegimePerformance* get_performance(
        const StrategyId& strategy_id,
        regime::DetailedRegime regime) const;

    /// Total number of recorded outcomes
    size_t total_outcomes() const;

    /// Reset all data
    void reset();

private:
    using Key = std::pair<std::string, regime::DetailedRegime>;

    struct KeyHash {
        // BUG-ML-18 fix: XOR with shift is a weak combiner for small enum values
        // and causes hash collisions. boost::hash_combine uses a proper mixing function.
        size_t operator()(const Key& k) const {
            size_t seed = std::hash<std::string>{}(k.first);
            boost::hash_combine(seed, static_cast<int>(k.second));
            return seed;
        }
    };

    void apply_decay(RegimePerformance& perf) const;

    RegimeEnsembleConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<Key, RegimePerformance, KeyHash> performance_;
    size_t total_outcomes_{0};
};

} // namespace tb::ml
