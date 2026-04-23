#pragma once
#include "risk/policies/i_risk_check.hpp"
#include "risk/state/risk_state.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <memory>

namespace tb::risk {

// ═══════════════════════════════════════════════════════════════
// Все реализации IRiskCheck
// ═══════════════════════════════════════════════════════════════

/// 1. Kill Switch / Emergency Halt
class KillSwitchCheck : public IRiskCheck {
public:
    KillSwitchCheck(RiskState& state) : state_(state) {}
    std::string_view name() const noexcept override { return "kill_switch_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    RiskState& state_;
};

/// 2. Day Lock check
class DayLockCheck : public IRiskCheck {
public:
    DayLockCheck(RiskState& state) : state_(state) {}
    std::string_view name() const noexcept override { return "day_lock_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    RiskState& state_;
};

/// 3. Symbol Lock check
class SymbolLockCheck : public IRiskCheck {
public:
    SymbolLockCheck(RiskState& state) : state_(state) {}
    std::string_view name() const noexcept override { return "symbol_lock_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    RiskState& state_;
};

/// 4. Strategy Lock check
class StrategyLockCheck : public IRiskCheck {
public:
    StrategyLockCheck(RiskState& state) : state_(state) {}
    std::string_view name() const noexcept override { return "strategy_lock_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    RiskState& state_;
};

/// 5. Symbol Cooldown check
class SymbolCooldownCheck : public IRiskCheck {
public:
    SymbolCooldownCheck(RiskState& state, std::shared_ptr<clock::IClock> clock)
        : state_(state), clock_(std::move(clock)) {}
    std::string_view name() const noexcept override { return "symbol_cooldown_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    RiskState& state_;
    std::shared_ptr<clock::IClock> clock_;
};

/// 6. Max Daily Loss
class DailyLossCheck : public IRiskCheck {
public:
    DailyLossCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "daily_loss_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 7. Realized Daily Loss
class RealizedDailyLossCheck : public IRiskCheck {
public:
    RealizedDailyLossCheck(const ExtendedRiskConfig& cfg, const RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "realized_daily_loss_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
};

/// 8. Max Drawdown
class MaxDrawdownCheck : public IRiskCheck {
public:
    MaxDrawdownCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "max_drawdown_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 9. Max Positions
class MaxPositionsCheck : public IRiskCheck {
public:
    MaxPositionsCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "max_positions_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 10. Same Direction Positions
class SameDirectionCheck : public IRiskCheck {
public:
    SameDirectionCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "same_direction_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 11. Max Exposure
class ExposureLimitCheck : public IRiskCheck {
public:
    ExposureLimitCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "exposure_limit_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 12. Max Notional (per-trade, with ReduceSize)
class PerTradeRiskCheck : public IRiskCheck {
public:
    PerTradeRiskCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "per_trade_risk_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 13. Max Leverage
class MaxLeverageCheck : public IRiskCheck {
public:
    MaxLeverageCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "max_leverage_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 14. Max Slippage
class MaxSlippageCheck : public IRiskCheck {
public:
    MaxSlippageCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "max_slippage_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 15. Consecutive Losses
class ConsecutiveLossesCheck : public IRiskCheck {
public:
    ConsecutiveLossesCheck(const ExtendedRiskConfig& cfg, const RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "consecutive_losses_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
};

/// 16. Per-Symbol Risk (concentration, daily loss per symbol, consecutive losses per symbol)
class PerSymbolRiskCheck : public IRiskCheck {
public:
    PerSymbolRiskCheck(const ExtendedRiskConfig& cfg, const RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "per_symbol_risk_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
};

/// 17. Per-Strategy Risk (budget, exposure)
class PerStrategyRiskCheck : public IRiskCheck {
public:
    PerStrategyRiskCheck(const ExtendedRiskConfig& cfg, const RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "per_strategy_risk_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
};

/// 18. Order Rate Limit
class OrderRateCheck : public IRiskCheck {
public:
    OrderRateCheck(const ExtendedRiskConfig& cfg, RiskState& state,
                   std::shared_ptr<clock::IClock> clock)
        : cfg_(cfg), state_(state), clock_(std::move(clock)) {}
    std::string_view name() const noexcept override { return "order_rate_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    RiskState& state_;
    std::shared_ptr<clock::IClock> clock_;
};

/// 19. Turnover Rate check
class TurnoverRateCheck : public IRiskCheck {
public:
    TurnoverRateCheck(const ExtendedRiskConfig& cfg, RiskState& state,
                      std::shared_ptr<clock::IClock> clock)
        : cfg_(cfg), state_(state), clock_(std::move(clock)) {}
    std::string_view name() const noexcept override { return "turnover_rate_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    RiskState& state_;
    std::shared_ptr<clock::IClock> clock_;
};

/// 20. Trade Interval check (per symbol)
class TradeIntervalCheck : public IRiskCheck {
public:
    TradeIntervalCheck(const ExtendedRiskConfig& cfg, const RiskState& state,
                       std::shared_ptr<clock::IClock> clock)
        : cfg_(cfg), state_(state), clock_(std::move(clock)) {}
    std::string_view name() const noexcept override { return "trade_interval_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
    std::shared_ptr<clock::IClock> clock_;
};

/// 21. Stale Feed check
class StaleFeedCheck : public IRiskCheck {
public:
    StaleFeedCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "stale_feed_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 22. Book Quality check
class BookQualityCheck : public IRiskCheck {
public:
    std::string_view name() const noexcept override { return "book_quality_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
};

/// 23. Spread check
class SpreadCheck : public IRiskCheck {
public:
    SpreadCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "spread_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 24. Liquidity check
class LiquidityCheck : public IRiskCheck {
public:
    LiquidityCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "liquidity_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 25. Max Loss Per Trade (existing positions losing too much)
class MaxLossPerTradeCheck : public IRiskCheck {
public:
    MaxLossPerTradeCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "max_loss_per_trade_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 26. UTC Cutoff check
class UtcCutoffCheck : public IRiskCheck {
public:
    UtcCutoffCheck(const ExtendedRiskConfig& cfg, std::shared_ptr<clock::IClock> clock)
        : cfg_(cfg), clock_(std::move(clock)) {}
    std::string_view name() const noexcept override { return "utc_cutoff_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    std::shared_ptr<clock::IClock> clock_;
};

/// 27. Regime Scaled Limits (ReduceSize under stress/chop)
class RegimeScaledLimitsCheck : public IRiskCheck {
public:
    RegimeScaledLimitsCheck(const ExtendedRiskConfig& cfg, const std::atomic<double>& scale)
        : cfg_(cfg), regime_scale_(scale) {}
    std::string_view name() const noexcept override { return "regime_scaled_limits_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const std::atomic<double>& regime_scale_;
};

/// 28. Uncertainty Limits (ReduceSize under high uncertainty)
class UncertaintyLimitsCheck : public IRiskCheck {
public:
    UncertaintyLimitsCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "uncertainty_limits_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 29. Uncertainty Cooldown
class UncertaintyCooldownCheck : public IRiskCheck {
public:
    std::string_view name() const noexcept override { return "uncertainty_cooldown_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
};

/// 30. Uncertainty Execution Mode (HaltNewEntries)
class UncertaintyExecutionModeCheck : public IRiskCheck {
public:
    std::string_view name() const noexcept override { return "uncertainty_execution_mode_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
};

/// 31. Intraday Drawdown check
class IntradayDrawdownCheck : public IRiskCheck {
public:
    IntradayDrawdownCheck(const ExtendedRiskConfig& cfg, const RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "intraday_drawdown_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    const RiskState& state_;
};

/// 32. Drawdown Hard Stop (emergency halt threshold)
class DrawdownHardStopCheck : public IRiskCheck {
public:
    DrawdownHardStopCheck(const ExtendedRiskConfig& cfg, RiskState& state)
        : cfg_(cfg), state_(state) {}
    std::string_view name() const noexcept override { return "drawdown_hard_stop_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
    RiskState& state_;
};

/// 33. Funding Rate Cost check — блокирует вход при экстремально дорогом фандинге
class FundingRateCostCheck : public IRiskCheck {
public:
    explicit FundingRateCostCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "funding_rate_cost_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 34. Venue Health check — blocks trading when venue is degraded
/// Monitors: REST latency, WS reconnects, reject rate, clock drift, fill gaps
class VenueHealthCheck : public IRiskCheck {
public:
    explicit VenueHealthCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "venue_health_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

/// 35. Margin Distance check — reduces size or blocks when close to liquidation
/// Ensures position + hedge margin stays within safe distance
class MarginDistanceCheck : public IRiskCheck {
public:
    explicit MarginDistanceCheck(const ExtendedRiskConfig& cfg) : cfg_(cfg) {}
    std::string_view name() const noexcept override { return "margin_distance_check"; }
    void evaluate(const RiskContext& ctx, RiskDecision& decision) override;
private:
    const ExtendedRiskConfig& cfg_;
};

} // namespace tb::risk
