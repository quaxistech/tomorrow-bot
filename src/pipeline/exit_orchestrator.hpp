#pragma once

#include "pipeline/exit_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

namespace tb::pipeline {

/// Единый владелец всех exit-решений.
/// Объединяет trailing stop, hard risk stops, market regime exits,
/// toxic flow, structural failure, continuation value model.
/// All exits are market-driven — no time-based gates in the alpha path.
/// Каждый выход имеет полный reason tree.
class PositionExitOrchestrator {
public:
    PositionExitOrchestrator(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock);

    /// Единая точка принятия exit-решения. Вызывается каждый тик для каждой позиции.
    /// @return ExitDecision с should_exit/should_reduce и полным explanation tree
    ExitDecision evaluate(const ExitContext& ctx) const;

    /// Обновить trailing stop state (highest/lowest/breakeven) и вернуть текущий stop level.
    /// Вызывается каждый тик — чистая функция обновления trailing state.
    struct TrailingUpdate {
        double stop_level{0.0};
        double trail_mult{2.0};
        bool breakeven_activated{false};
        double highest{0.0};
        double lowest{0.0};
    };
    TrailingUpdate update_trailing(const ExitContext& ctx) const;

private:
    // --- Hard stops (safety net, cannot be overridden) ---
    ExitDecision check_fixed_capital_stop(const ExitContext& ctx) const;
    ExitDecision check_price_stop(const ExitContext& ctx) const;

    // --- Trailing / Chandelier ---
    ExitDecision check_trailing_stop(const ExitContext& ctx) const;

    // --- Quick profit (fee-aware) ---
    ExitDecision check_quick_profit(const ExitContext& ctx, const ContinuationState& state) const;

    // --- Partial take-profit (market-aware) ---
    ExitDecision check_partial_tp(const ExitContext& ctx, const ContinuationState& state) const;

    // --- Continuation value model (heart of the system) ---
    ContinuationState compute_continuation_state(const ExitContext& ctx) const;
    ExitDecision check_continuation_value_exit(const ExitContext& ctx, const ContinuationState& state) const;

    // --- Structural exits ---
    ExitDecision check_toxic_flow(const ExitContext& ctx) const;
    ExitDecision check_structural_failure(const ExitContext& ctx) const;
    ExitDecision check_liquidity_deterioration(const ExitContext& ctx, const ContinuationState& state) const;

    // --- Market-driven exits (Phase 2) ---
    ExitDecision check_market_regime_exit(const ExitContext& ctx, const ContinuationState& state) const;
    ExitDecision check_funding_carry_exit(const ExitContext& ctx) const;
    ExitDecision check_stale_data_exit(const ExitContext& ctx) const;

    // --- Reason tree builder ---
    ExitExplanation build_explanation(ExitSignalType signal,
                                     const std::string& primary,
                                     const ContinuationState& state,
                                     const std::string& counterfactual) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
};

} // namespace tb::pipeline
