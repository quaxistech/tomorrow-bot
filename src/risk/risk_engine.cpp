#include "risk/risk_engine.hpp"
#include "risk/policies/risk_checks.hpp"
#include "strategy/strategy_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "order_book/order_book_types.hpp"
#include "common/constants.hpp"
#include "common/enums.hpp"
#include "common/types.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace tb::risk {

// ═══════════════════════════════════════════════════════════════
// Конструктор — инициализирует цепочку policy-проверок
// ═══════════════════════════════════════════════════════════════

ProductionRiskEngine::ProductionRiskEngine(
    ExtendedRiskConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    init_checks();
}

void ProductionRiskEngine::init_checks() {
    checks_.clear();
    checks_.reserve(33);

    // === Блокировки (hard deny, порядок важен) ===
    checks_.push_back(std::make_unique<KillSwitchCheck>(state_));           // 1
    checks_.push_back(std::make_unique<DayLockCheck>(state_));              // 2
    checks_.push_back(std::make_unique<SymbolLockCheck>(state_));           // 3
    checks_.push_back(std::make_unique<StrategyLockCheck>(state_));         // 4
    checks_.push_back(std::make_unique<SymbolCooldownCheck>(state_, clock_)); // 5

    // === Дневные лимиты ===
    checks_.push_back(std::make_unique<DailyLossCheck>(config_));           // 6
    checks_.push_back(std::make_unique<RealizedDailyLossCheck>(config_, state_)); // 7
    checks_.push_back(std::make_unique<MaxDrawdownCheck>(config_));         // 8
    checks_.push_back(std::make_unique<IntradayDrawdownCheck>(config_, state_)); // 32→9
    checks_.push_back(std::make_unique<DrawdownHardStopCheck>(config_, state_)); // 33→10

    // === Позиционные лимиты ===
    checks_.push_back(std::make_unique<MaxPositionsCheck>(config_));        // 11
    checks_.push_back(std::make_unique<SameDirectionCheck>(config_));       // 12
    checks_.push_back(std::make_unique<ExposureLimitCheck>(config_));       // 13
    checks_.push_back(std::make_unique<PerTradeRiskCheck>(config_));        // 14
    checks_.push_back(std::make_unique<MaxLeverageCheck>(config_));         // 15
    checks_.push_back(std::make_unique<MaxSlippageCheck>(config_));         // 16

    // === Серии убытков ===
    checks_.push_back(std::make_unique<ConsecutiveLossesCheck>(config_, state_));  // 17
    checks_.push_back(std::make_unique<MaxLossPerTradeCheck>(config_));            // 18

    // === Per-symbol / per-strategy ===
    checks_.push_back(std::make_unique<PerSymbolRiskCheck>(config_, state_));      // 19
    checks_.push_back(std::make_unique<PerStrategyRiskCheck>(config_, state_));    // 20

    // === Rate limiting ===
    checks_.push_back(std::make_unique<OrderRateCheck>(config_, state_, clock_));      // 21
    checks_.push_back(std::make_unique<TurnoverRateCheck>(config_, state_, clock_));   // 22
    checks_.push_back(std::make_unique<TradeIntervalCheck>(config_, state_, clock_));  // 23

    // === Market conditions ===
    checks_.push_back(std::make_unique<StaleFeedCheck>(config_));           // 24
    checks_.push_back(std::make_unique<BookQualityCheck>());                // 25
    checks_.push_back(std::make_unique<SpreadCheck>(config_));              // 26
    checks_.push_back(std::make_unique<LiquidityCheck>(config_));           // 27

    // === Timing / Regime ===
    checks_.push_back(std::make_unique<UtcCutoffCheck>(config_, clock_));   // 28
    checks_.push_back(std::make_unique<RegimeScaledLimitsCheck>(config_, regime_scale_factor_)); // 29

    // === Uncertainty ===
    checks_.push_back(std::make_unique<UncertaintyLimitsCheck>(config_));   // 30
    checks_.push_back(std::make_unique<UncertaintyCooldownCheck>());        // 31
    checks_.push_back(std::make_unique<UncertaintyExecutionModeCheck>());   // 32

    // === Funding cost ===
    checks_.push_back(std::make_unique<FundingRateCostCheck>(config_));     // 33

    // === Phase 5: Venue-risk and margin-aware ===
    checks_.push_back(std::make_unique<VenueHealthCheck>(config_));        // 34
    checks_.push_back(std::make_unique<MarginDistanceCheck>(config_));     // 35
}

// ═══════════════════════════════════════════════════════════════
// evaluate — оркестрирует все policy-проверки
// ═══════════════════════════════════════════════════════════════

RiskDecision ProductionRiskEngine::evaluate(
    const strategy::TradeIntent& intent,
    const portfolio_allocator::SizingResult& sizing,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& features,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    std::lock_guard lock(mutex_);

    // Обновить equity в drawdown tracker
    const double current_equity = portfolio.total_capital + portfolio.pnl.total_pnl;
    state_.drawdown.update_equity(current_equity, clock_->now());

    // Очистить истёкшие блокировки
    state_.locks.clear_expired(clock_->now());

    // Подготовить решение
    RiskDecision decision;
    decision.decided_at = clock_->now();
    decision.approved_quantity = sizing.approved_quantity;
    decision.original_size = sizing.approved_quantity;

    // BUG-S17-07: NaN/Inf quantity from allocator must be rejected before checks run.
    if (!std::isfinite(sizing.approved_quantity.get()) || sizing.approved_quantity.get() < 0.0) {
        decision.verdict = RiskVerdict::Denied;
        decision.allowed = false;
        decision.action = RiskAction::Deny;
        decision.reasons.push_back(RiskReasonCode{
            "INVALID_SIZE",
            "approved_quantity NaN/Inf/negative from allocator",
            1.0
        });
        return decision;
    }

    decision.verdict = RiskVerdict::Approved;
    decision.action = RiskAction::Allow;
    decision.allowed = true;
    decision.phase = RiskPhase::PreTrade;

    // Собрать контекст
    RiskContext ctx{intent, sizing, portfolio, features, exec_alpha, uncertainty,
                    current_funding_rate_.load(std::memory_order_acquire),
                    min_notional_usdt_.load(std::memory_order_acquire)};

    // Фьючерсная безопасность: закрытие позиций НЕ блокируется risk engine,
    // но safety-критичные проверки выполняются для мониторинга.
    if (intent.trade_side == TradeSide::Close) {
        // BUG-S13-09: also run leverage and drawdown checks for close trades so
        // extreme conditions are logged and monitored — close is still always approved.
        static constexpr std::array<std::string_view, 5> close_check_names = {
            "order_rate_check", "max_slippage_check", "stale_feed_check",
            "max_leverage_check", "max_drawdown_check"
        };
        for (auto& check : checks_) {
            for (auto allowed_name : close_check_names) {
                if (check->name() == allowed_name) {
                    check->evaluate(ctx, decision);
                    break;
                }
            }
        }
        // Close всегда разрешён — не блокируем выход из позиции
        decision.verdict = RiskVerdict::Approved;
        decision.action = RiskAction::Allow;
        decision.allowed = true;
        decision.summary = "Close: safety checks executed, exit not blocked (futures safety)";
        finalize_decision(decision, intent, sizing, portfolio);
        return decision;
    }

    // Прогнать все 33 проверки в порядке приоритета (только для Open)
    for (auto& check : checks_) {
        check->evaluate(ctx, decision);
    }

    // Синхронизировать action ↔ verdict
    if (decision.verdict == RiskVerdict::Denied) {
        decision.allowed = false;
        if (decision.action == RiskAction::Allow ||
            decision.action == RiskAction::AllowWithReducedSize) {
            decision.action = RiskAction::Deny;
        }
    } else if (decision.verdict == RiskVerdict::Throttled) {
        decision.allowed = false;
    } else if (decision.verdict == RiskVerdict::ReduceSize) {
        decision.action = RiskAction::AllowWithReducedSize;
    } else {
        decision.allowed = true;
    }

    // Финализация: min notional guard, summary, metrics
    finalize_decision(decision, intent, sizing, portfolio);

    return decision;
}

void ProductionRiskEngine::finalize_decision(
    RiskDecision& decision,
    const strategy::TradeIntent& intent,
    const portfolio_allocator::SizingResult& sizing,
    const portfolio::PortfolioSnapshot& portfolio)
{
    // Per-symbol min notional: используем реальный минимум, если задан, иначе fallback
    const double min_notional = min_notional_usdt_.load(std::memory_order_acquire) > 0.0
        ? min_notional_usdt_.load(std::memory_order_acquire)
        : common::exchange_limits::kMinBitgetNotionalUsdt;

    // Min notional guard: если ReduceSize уменьшил объём ниже биржевого минимума
    if (decision.verdict == RiskVerdict::ReduceSize &&
        sizing.approved_quantity.get() > 0.0) {
        double price_per_unit = sizing.approved_notional.get() / sizing.approved_quantity.get();
        double reduced_notional = decision.approved_quantity.get() * price_per_unit;
        if (reduced_notional < min_notional) {
            decision.verdict = RiskVerdict::Denied;
            decision.allowed = false;
            decision.action = RiskAction::Deny;
            decision.reasons.push_back(RiskReasonCode{
                "REDUCE_BELOW_MIN_NOTIONAL",
                "Уменьшенный объём " + std::to_string(reduced_notional) +
                " USDT ниже минимума " + std::to_string(min_notional),
                1.0
            });
        }
    }

    // Risk utilization
    if (portfolio.total_capital > 0.0) {
        decision.risk_utilization_pct =
            portfolio.exposure.gross_exposure / portfolio.total_capital;
        decision.current_gross_exposure = portfolio.exposure.gross_exposure;
        decision.current_drawdown_pct = portfolio.pnl.current_drawdown_pct;
        decision.current_daily_pnl = portfolio.pnl.total_pnl;
    }

    // Regime factor — BUG-S17-01: validate before use; NaN/OOB factor makes all
    // regime-scaled limits NaN, silently disabling risk checks.
    {
        double factor = regime_scale_factor_.load(std::memory_order_acquire);
        if (!std::isfinite(factor) || factor < 0.1 || factor > 3.0) factor = 1.0;
        decision.regime_scaling_factor = factor;
    }

    // Global risk state
    decision.risk_state = state_.locks.compute_global_state();
    decision.active_locks_count = state_.locks.count();

    // Summary
    std::ostringstream oss;
    oss << "Вердикт=" << to_string(decision.verdict)
        << " action=" << to_string(decision.action)
        << " причины=" << decision.reasons.size()
        << " triggered=" << decision.triggered_checks.size()
        << " утилизация=" << decision.risk_utilization_pct;
    decision.summary = oss.str();

    // Logging
    if (decision.verdict != RiskVerdict::Approved) {
        std::string codes;
        for (const auto& r : decision.reasons) {
            if (!codes.empty()) codes += ",";
            codes += r.code;
        }
        logger_->warn("RiskEngine", "Ордер не одобрен",
                      {{"symbol", intent.symbol.get()},
                       {"verdict", to_string(decision.verdict)},
                       {"action", to_string(decision.action)},
                       {"codes", codes},
                       {"triggered", std::to_string(decision.triggered_checks.size())},
                       {"hard_blocks", std::to_string(decision.hard_blocks.size())}});
    }

    if (metrics_) {
        metrics_->counter("risk_evaluations_total",
                          {{"verdict", to_string(decision.verdict)},
                           {"action", to_string(decision.action)}})->increment();
    }
}

// ═══════════════════════════════════════════════════════════════
// Kill switch
// ═══════════════════════════════════════════════════════════════

void ProductionRiskEngine::activate_kill_switch(const std::string& reason) {
    std::lock_guard lock(mutex_);
    kill_switch_active_.store(true);
    kill_switch_reason_ = reason;

    // Отразить в state
    state_.locks.add_lock(LockType::EmergencyHalt, "", reason, clock_->now());

    logger_->critical("RiskEngine", "АВАРИЙНЫЙ ВЫКЛЮЧАТЕЛЬ АКТИВИРОВАН",
                      {{"reason", reason}});
}

void ProductionRiskEngine::deactivate_kill_switch() {
    std::lock_guard lock(mutex_);
    kill_switch_active_.store(false);
    kill_switch_reason_.clear();

    // Убрать из state
    state_.locks.remove_lock(LockType::EmergencyHalt, "");

    logger_->info("RiskEngine", "Аварийный выключатель деактивирован", {});
}

bool ProductionRiskEngine::is_kill_switch_active() const {
    return kill_switch_active_.load();
}

// ═══════════════════════════════════════════════════════════════
// Event recording
// ═══════════════════════════════════════════════════════════════

void ProductionRiskEngine::record_order_sent() {
    std::lock_guard lock(mutex_);
    state_.rates.record_order(clock_->now());
}

void ProductionRiskEngine::record_trade_result(bool /*is_loss*/) {
    // Намеренно пустой: реальный учёт выполняется в record_trade_close()
    // с полными деталями (strategy_id, symbol, realized_pnl).
    // Этот метод сохранён для обратной совместимости IRiskEngine.
}

void ProductionRiskEngine::record_trade_close(const StrategyId& strategy_id,
                                               const Symbol& symbol,
                                               double realized_pnl)
{
    std::lock_guard lock(mutex_);
    const bool is_loss = realized_pnl < 0.0;
    const auto now = clock_->now();

    // Обновить трекеры
    state_.loss_streaks.record_trade_result(symbol.get(), strategy_id.get(), is_loss, now);
    state_.pnl.record_trade_pnl(symbol.get(), strategy_id.get(), realized_pnl);
    state_.rates.record_trade_close(now);
    state_.rates.record_symbol_trade(symbol.get(), now);

    // Обновить бюджет стратегии
    auto& budget = state_.strategy_budgets[strategy_id.get()];
    budget.strategy_id = strategy_id;
    budget.trades_today++;
    budget.last_trade_at = now;

    if (is_loss) {
        budget.daily_loss += std::abs(realized_pnl);
        budget.consecutive_losses++;
    } else {
        budget.consecutive_losses = 0;
    }

    // Автоматические блокировки на основе правил
    // Symbol cooldown после N стопаутов
    int symbol_losses = state_.loss_streaks.symbol_consecutive_losses(symbol.get());
    if (symbol_losses >= config_.max_consecutive_losses_per_symbol) {
        state_.locks.add_lock(LockType::Cooldown, symbol.get(),
                              "consecutive_losses_per_symbol=" + std::to_string(symbol_losses),
                              now, config_.symbol_cooldown_after_stopouts_ns);
        logger_->warn("RiskEngine", "Symbol cooldown активирован",
                      {{"symbol", symbol.get()},
                       {"losses", std::to_string(symbol_losses)}});
    }

    // Loss cooldown после N подряд убытков (глобальный)
    int total_losses = state_.loss_streaks.total_consecutive_losses();
    if (total_losses >= config_.cooldown_after_n_losses &&
        total_losses < config_.halt_after_n_losses) {
        state_.locks.add_lock(LockType::Cooldown, "",
                              "consecutive_losses_global=" + std::to_string(total_losses),
                              now, config_.loss_cooldown_ns);
    }

    // Halt после N подряд убытков
    if (total_losses >= config_.halt_after_n_losses) {
        state_.locks.add_lock(LockType::DayLock, "",
                              "halt_after_" + std::to_string(config_.halt_after_n_losses) + "_losses",
                              now);
        logger_->critical("RiskEngine", "Day lock: слишком много убытков подряд",
                          {{"total_losses", std::to_string(total_losses)}});
    }
}

// ═══════════════════════════════════════════════════════════════
// Intra-trade monitoring
// ═══════════════════════════════════════════════════════════════

IntraTradeAssessment ProductionRiskEngine::evaluate_position(
    const portfolio::Position& position,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& features)
{
    IntraTradeAssessment assessment;
    assessment.symbol = position.symbol;
    assessment.assessed_at = clock_->now();

    // max_adverse_excursion_pct — safety exit (RiskKill category)
    if (portfolio.total_capital > 0.0 && position.unrealized_pnl < 0.0) {
        const double adverse_pct =
            std::abs(position.unrealized_pnl) / portfolio.total_capital * 100.0;
        if (adverse_pct >= config_.max_adverse_excursion_pct) {
            assessment.should_close = true;
            assessment.reasons.push_back({
                "MAX_ADVERSE_EXCURSION",
                "Adverse excursion " + std::to_string(adverse_pct) +
                "% >= limit " + std::to_string(config_.max_adverse_excursion_pct) + "%",
                1.0
            });
        }
    }

    // Auto-trigger kill switch when portfolio drawdown breaches the portfolio-level threshold.
    // kill_switch_portfolio_drawdown_pct is a portfolio-wide stop (default 6%) — separate from
    // max_adverse_excursion_pct (per-position MAE exit, default 3%). The portfolio threshold is
    // intentionally larger: it catches cascading losses across multiple positions that individual
    // MAE checks do not see. Once activated, no new trades can open; only exit/flatten orders
    // are allowed through.
    if (portfolio.pnl.current_drawdown_pct >= config_.kill_switch_portfolio_drawdown_pct) {
        if (!kill_switch_active_.load()) {
            activate_kill_switch(
                "AUTO: Portfolio drawdown " +
                std::to_string(portfolio.pnl.current_drawdown_pct) +
                "% >= portfolio kill-switch threshold " +
                std::to_string(config_.kill_switch_portfolio_drawdown_pct) + "%");
        }
        assessment.should_close = true;
        assessment.reasons.push_back({
            "LIQUIDATION_PROXIMITY",
            "Portfolio drawdown " + std::to_string(portfolio.pnl.current_drawdown_pct) +
            "% - kill switch auto-triggered",
            1.0
        });
    }

    // Phase 5: Operational deadman watchdog
    // Only fires when degradation conditions are present, not on pure elapsed time.
    // Degradation = stale feed + runaway exposure + health check failure.
    // Pure time-based exit is an alpha decision (handled by exit_orchestrator).
    // BUG-S35-04: NTP backward jump makes hold_ns negative → deadman never fires.
    const int64_t hold_ns = std::max(int64_t{0}, clock_->now().get() - position.opened_at.get());
    if (config_.operational_deadman_ns > 0 && hold_ns > config_.operational_deadman_ns) {
        // Count degradation signals
        int degradation_count = 0;
        std::string degradation_detail;

        // 1. Stale market data
        if (features.execution_context.is_feed_fresh == false) {
            ++degradation_count;
            degradation_detail += "stale_feed ";
        }

        // 2. Wide spread = possible liquidity degradation
        if (features.microstructure.spread_valid && features.microstructure.spread_bps > 50.0) {
            ++degradation_count;
            degradation_detail += "wide_spread(" + std::to_string(features.microstructure.spread_bps) + "bps) ";
        }

        // 3. Runaway loss — position losing more than expected
        if (portfolio.total_capital > 0.0 && position.unrealized_pnl < 0.0) {
            double loss_pct = std::abs(position.unrealized_pnl) / portfolio.total_capital * 100.0;
            if (loss_pct > config_.max_adverse_excursion_pct * 0.8) {
                ++degradation_count;
                degradation_detail += "loss_near_mae(" + std::to_string(loss_pct) + "%) ";
            }
        }

        // 4. VPIN toxicity = adverse flow environment
        if (features.microstructure.vpin_valid && features.microstructure.vpin_toxic) {
            ++degradation_count;
            degradation_detail += "vpin_toxic ";
        }

        // Phase C: Fire only if structural degradation is confirmed (2+ signals).
        // Pure time-based absolute_deadman removed — time alone is not alpha.
        // Operational deadman requires at least 1 degradation signal.
        bool structural_degradation = degradation_count >= 2;

        if (structural_degradation) {
            assessment.should_close = true;
            assessment.reasons.push_back({
                "DEADMAN_WATCHDOG",
                "Degraded execution: "
                + degradation_detail + "hold=" +
                std::to_string(hold_ns / 1'000'000'000LL) + "s",
                0.8
            });
        }
    }

    // All assessments from risk engine are safety exits (not alpha)
    if (assessment.should_close) {
        assessment.is_safety_exit = true;
    }

    return assessment;
}

// ═══════════════════════════════════════════════════════════════
// Regime
// ═══════════════════════════════════════════════════════════════

void ProductionRiskEngine::set_current_regime(regime::DetailedRegime regime) {
    std::lock_guard lock(mutex_);
    current_regime_ = regime;

    if (!config_.regime_aware_limits_enabled) {
        regime_scale_factor_.store(1.0, std::memory_order_release);
        return;
    }

    double scale = 1.0;
    switch (regime) {
        case regime::DetailedRegime::LiquidityStress:
        case regime::DetailedRegime::VolatilityExpansion:
        case regime::DetailedRegime::AnomalyEvent:
        case regime::DetailedRegime::ToxicFlow:
        case regime::DetailedRegime::SpreadInstability:
            scale = config_.stress_regime_scale;
            break;

        case regime::DetailedRegime::StrongUptrend:
        case regime::DetailedRegime::StrongDowntrend:
            scale = config_.trending_regime_scale;
            break;

        case regime::DetailedRegime::Chop:
        case regime::DetailedRegime::LowVolCompression:
            scale = config_.chop_regime_scale;
            break;

        default:
            scale = 1.0;
            break;
    }
    regime_scale_factor_.store(scale, std::memory_order_release);
}

void ProductionRiskEngine::set_funding_rate(double rate) {
    current_funding_rate_.store(rate, std::memory_order_release);
}

void ProductionRiskEngine::set_min_notional_usdt(double value) {
    min_notional_usdt_.store(value, std::memory_order_release);
}

// ═══════════════════════════════════════════════════════════════
// Observability
// ═══════════════════════════════════════════════════════════════

RiskSnapshot ProductionRiskEngine::get_risk_snapshot() const {
    std::lock_guard lock(mutex_);

    RiskSnapshot snap;
    snap.computed_at = clock_->now();
    snap.kill_switch_active = kill_switch_active_.load();
    snap.regime_scaling_factor = regime_scale_factor_.load(std::memory_order_acquire);
    snap.global_state = state_.locks.compute_global_state();
    snap.active_locks = state_.locks.active_locks();
    snap.current_drawdown_pct = state_.drawdown.account_drawdown_pct();
    snap.open_positions = 0;  // Заполняется pipeline из portfolio
    snap.daily_loss_pct = 0.0; // Заполняется pipeline из portfolio
    snap.gross_exposure_pct = 0.0; // Заполняется pipeline из portfolio
    snap.total_risk_utilization = 0.0; // Заполняется pipeline из portfolio
    snap.rules_triggered = 0;  // Заполняется при evaluate()

    for (const auto& [key, budget] : state_.strategy_budgets) {
        snap.strategy_budgets.push_back(budget);
    }

    return snap;
}

void ProductionRiskEngine::reset_daily() {
    std::lock_guard lock(mutex_);
    state_.reset_daily(clock_->now());
    // BUG-S13-14: regime_scale_factor_ is not reset with the daily cycle,
    // causing stale regime limits to persist into the next trading day.
    regime_scale_factor_.store(1.0, std::memory_order_relaxed);
}

} // namespace tb::risk
