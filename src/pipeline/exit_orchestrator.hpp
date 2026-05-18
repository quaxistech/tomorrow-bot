#pragma once

#include "pipeline/exit_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

namespace tb::pipeline {

/// Emergency-tier exit safety net (edge-31 Phase 5).
///
/// Этот orchestrator теперь отвечает ТОЛЬКО за emergency exits, которые
/// невозможно реализовать через биржевой TP/SL bracket:
///   1) hard_capital_stop — последняя защита от catastrophic loss
///   2) toxic_flow — токсичная микроструктура → fast exit
///   3) stale_data — feed протух → не торгуем вслепую
///
/// Основная защита позиции лежит на двух слоях:
///   • Layer 1 — exchange-attached TP/SL (presetStop* + ProtectiveBracketManager)
///   • Layer 2 — adaptive trailing через bracket_manager_->update_sl()
///
/// Метод update_trailing() остаётся — он вычисляет новый уровень SL для Phase 4
/// push'а на биржу. Локальные close-on-trailing-breach trigger'ы удалены.
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
    // edge-31 Phase 5: Только три emergency-tier exit checks остаются.
    // Все alpha-tier exits удалены — их функцию закрывает exchange TP/SL + trailing.
    //
    // --- Hard capital stop (safety net на случай отказа exchange SL) ---
    ExitDecision check_fixed_capital_stop(const ExitContext& ctx) const;
    // --- Toxic flow emergency ---
    ExitDecision check_toxic_flow(const ExitContext& ctx) const;
    // --- Stale data emergency ---
    ExitDecision check_stale_data_exit(const ExitContext& ctx) const;

    /// Помощник для построения reason tree (используется emergency-tier exits).
    ExitExplanation build_explanation(ExitSignalType signal,
                                       const std::string& primary,
                                       const ContinuationState& state,
                                       const std::string& counterfactual) const;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
};

} // namespace tb::pipeline
