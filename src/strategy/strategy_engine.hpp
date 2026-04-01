#pragma once
/**
 * @file strategy_engine.hpp
 * @brief Strategy Engine — единственная скальпинговая стратегия (§1-4 ТЗ)
 *
 * Реализует IStrategy для обратной совместимости с pipeline.
 * Внутри управляет state machine, сетапами, позициями и выходами.
 */

#include "strategy/strategy_interface.hpp"
#include "strategy/strategy_config.hpp"
#include "strategy/state/strategy_state.hpp"
#include "strategy/context/market_context.hpp"
#include "strategy/setups/setup_lifecycle.hpp"
#include "strategy/management/position_manager.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>

namespace tb::strategy {

/// Strategy Engine — центральный модуль принятия торговых решений
///
/// Реализует одну скальпинговую стратегию с 4 внутренними под-сценариями:
/// 1. Momentum Continuation Scalp
/// 2. Retest Scalp
/// 3. Pullback in Microtrend
/// 4. Rejection Scalp
///
/// Каждый вызов evaluate() проводит полный цикл:
///   context → state machine → setup detection/validation → entry/exit → signal
class StrategyEngine : public IStrategy {
public:
    StrategyEngine(std::shared_ptr<logging::ILogger> logger,
                   std::shared_ptr<clock::IClock> clock,
                   ScalpStrategyConfig cfg = {});

    // ─── IStrategy ───────────────────────────────────────────────────────
    StrategyMeta meta() const override;
    std::optional<TradeIntent> evaluate(const StrategyContext& context) override;
    bool is_active() const override;
    void set_active(bool active) override;
    void reset() override;

    // ─── Strategy Engine API ─────────────────────────────────────────────

    /// Уведомить о фактическом открытии позиции (feedback от pipeline)
    void notify_position_opened(double entry_price, double size,
                                Side side, PositionSide pos_side);

    /// Уведомить о закрытии позиции
    void notify_position_closed();

    /// Текущее состояние state machine
    SymbolState current_state() const;

    /// Текстовый отчёт о последнем решении (для explainability)
    const std::vector<std::string>& last_decision_reasons() const { return last_reasons_; }

private:
    // ─── Фазы evaluate() ─────────────────────────────────────────────────

    /// Обработка cooldown состояния
    std::optional<TradeIntent> handle_cooldown(const StrategyContext& ctx, int64_t now_ns);

    /// Обработка состояний без позиции (Idle → EntryReady)
    std::optional<TradeIntent> handle_pre_entry(const StrategyContext& ctx, int64_t now_ns);

    /// Обработка состояний с позицией (PositionOpen, PositionManaging)
    std::optional<TradeIntent> handle_position(const StrategyContext& ctx, int64_t now_ns);

    /// Построить TradeIntent из решения стратегии
    TradeIntent build_intent(const StrategyContext& ctx, const Setup& setup,
                             StrategySignalType signal_type, int64_t now_ns) const;

    /// Построить TradeIntent для выхода
    TradeIntent build_exit_intent(const StrategyContext& ctx,
                                  const PositionManagementResult& mgmt,
                                  int64_t now_ns) const;

    /// Отменить текущий сетап и перейти в cooldown
    void cancel_setup(int64_t now_ns, const std::string& reason);

    // ─── Компоненты ──────────────────────────────────────────────────────
    ScalpStrategyConfig cfg_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    StrategyStateMachine state_machine_;
    MarketContextEvaluator context_eval_;
    SetupDetector setup_detector_;
    SetupValidator setup_validator_;
    PositionManager position_manager_;

    std::atomic<bool> active_{true};
    std::vector<std::string> last_reasons_;
};

} // namespace tb::strategy
