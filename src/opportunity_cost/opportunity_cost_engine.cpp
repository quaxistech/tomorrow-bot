#include "opportunity_cost/opportunity_cost_engine.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace tb::opportunity_cost {

// ─── Конструктор ─────────────────────────────────────────────────────────────

RuleBasedOpportunityCost::RuleBasedOpportunityCost(
    OpportunityCostConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        actions_execute_  = metrics_->counter("opportunity_cost_actions_total", {{"action", "execute"}});
        actions_defer_    = metrics_->counter("opportunity_cost_actions_total", {{"action", "defer"}});
        actions_suppress_ = metrics_->counter("opportunity_cost_actions_total", {{"action", "suppress"}});
        actions_upgrade_  = metrics_->counter("opportunity_cost_actions_total", {{"action", "upgrade"}});
        last_net_edge_bps_ = metrics_->gauge("opportunity_cost_net_edge_bps", {});
        last_score_        = metrics_->gauge("opportunity_cost_score", {});
        decision_latency_  = metrics_->histogram("opportunity_cost_decision_latency_ns",
            {100, 500, 1000, 5000, 10000, 50000, 100000}, {});
    }
}

// ─── Основной метод ──────────────────────────────────────────────────────────

OpportunityCostResult RuleBasedOpportunityCost::evaluate(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const PortfolioContext& portfolio_ctx,
    double conviction_threshold)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto start = clock_->now();

    OpportunityCostResult result;
    result.computed_at = start;
    result.budget_utilization = portfolio_ctx.gross_exposure_pct;

    // 1. Скоринг с полной декомпозицией
    result.score = compute_score(intent, exec_alpha, portfolio_ctx);

    // 2. Определить действие и причину
    auto [action, reason] = determine_action(
        result.score, portfolio_ctx, intent.conviction, conviction_threshold);
    result.action = action;
    result.reason = reason;

    // 3. Ранг по умолчанию (при одиночной оценке = 1)
    result.rank = 1;

    // 4. Построить аудит-трассу
    result.factors = build_factors(
        intent, exec_alpha, portfolio_ctx, conviction_threshold,
        result.score, action, reason);

    // 5. Машиночитаемые reason codes
    result.reason_codes.push_back(to_string(action));
    result.reason_codes.push_back(to_string(reason));
    if (result.factors.concentration_limited) result.reason_codes.push_back("ConcentrationLimited");
    if (result.factors.capital_limited) result.reason_codes.push_back("CapitalLimited");
    if (result.factors.drawdown_penalized) result.reason_codes.push_back("DrawdownPenalized");

    // 6. Человекочитаемое обоснование
    std::ostringstream oss;
    oss << "action=" << to_string(action)
        << " reason=" << to_string(reason)
        << " rule=" << result.factors.rule_id
        << " net_bps=" << std::fixed << std::setprecision(1) << result.score.net_expected_bps
        << " score=" << std::setprecision(3) << result.score.score
        << " efficiency=" << std::setprecision(2) << result.score.capital_efficiency
        << " exposure=" << std::setprecision(2) << portfolio_ctx.gross_exposure_pct;
    result.rationale = oss.str();

    // 7. Метрики
    if (metrics_) {
        switch (action) {
            case OpportunityAction::Execute:  actions_execute_->increment(); break;
            case OpportunityAction::Defer:    actions_defer_->increment(); break;
            case OpportunityAction::Suppress: actions_suppress_->increment(); break;
            case OpportunityAction::Upgrade:  actions_upgrade_->increment(); break;
        }
        last_net_edge_bps_->set(result.score.net_expected_bps);
        last_score_->set(result.score.score);

        auto elapsed = clock_->now().get() - start.get();
        decision_latency_->observe(static_cast<double>(elapsed));
    }

    // 8. Логирование
    logger_->debug("OpportunityCost", "Оценка возможности",
                   {{"symbol", intent.symbol.get()},
                    {"action", to_string(action)},
                    {"reason", to_string(reason)},
                    {"rule", std::to_string(result.factors.rule_id)},
                    {"net_bps", std::to_string(result.score.net_expected_bps)},
                    {"score", std::to_string(result.score.score)},
                    {"conviction", std::to_string(intent.conviction)},
                    {"exposure", std::to_string(portfolio_ctx.gross_exposure_pct)}});

    return result;
}

// ─── Скоринг ─────────────────────────────────────────────────────────────────

OpportunityScore RuleBasedOpportunityCost::compute_score(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const PortfolioContext& portfolio_ctx) const
{
    OpportunityScore score;

    // BUG-S16-05: NaN conviction → clamp(NaN)=NaN → pow(NaN)=NaN → all scoring NaN
    // → net_expected_bps < threshold always false → all trades "approved"
    if (!std::isfinite(intent.conviction)) {
        score.expected_return_bps = 0.0;
        score.net_expected_bps    = -score.execution_cost_bps;
        return score;
    }

    // Expected return: conviction → bps через конфигурируемый масштаб.
    // Нелинейная зависимость: conviction^1.5 усиливает разделение
    // между слабыми и сильными сигналами.
    const double conviction_pow = std::pow(std::clamp(intent.conviction, 0.0, 1.0), 1.5);
    score.expected_return_bps = conviction_pow * config_.conviction_to_bps_scale;

    // Стоимость исполнения из ExecutionAlpha
    score.execution_cost_bps = exec_alpha.quality.total_cost_bps;

    // Чистый ожидаемый доход
    score.net_expected_bps = score.expected_return_bps - score.execution_cost_bps;

    // Эффективность использования капитала:
    // net edge, нормализованный на текущую экспозицию.
    // Мягкий пол (5%) предотвращает деление на ноль и стимулирует
    // выход на рынок при низкой загруженности.
    const double exposure_floor = std::max(portfolio_ctx.gross_exposure_pct, 0.05);
    score.capital_efficiency = score.net_expected_bps / exposure_floor;

    // Компоненты composite score (для аудита)
    score.conviction_component = std::clamp(intent.conviction, 0.0, 1.0);
    score.net_edge_component = std::clamp(
        score.net_expected_bps / config_.conviction_to_bps_scale, 0.0, 1.0);
    score.capital_efficiency_component = std::clamp(
        score.capital_efficiency / config_.conviction_to_bps_scale, 0.0, 1.0);
    score.urgency_component = std::clamp(intent.urgency, 0.0, 1.0);

    // Composite score: взвешенная сумма, нормализованная к [0, 1]
    score.score = std::clamp(
        config_.weight_conviction * score.conviction_component +
        config_.weight_net_edge * score.net_edge_component +
        config_.weight_capital_efficiency * score.capital_efficiency_component +
        config_.weight_urgency * score.urgency_component,
        0.0, 1.0);

    return score;
}

// ─── Определение действия ────────────────────────────────────────────────────

std::pair<OpportunityAction, OpportunityReason>
RuleBasedOpportunityCost::determine_action(
    const OpportunityScore& score,
    const PortfolioContext& portfolio_ctx,
    double conviction,
    double conviction_threshold) const
{
    const double eff_threshold = effective_conviction_threshold(
        conviction_threshold, portfolio_ctx);

    // Правило 1: Отрицательный чистый доход → подавить
    if (score.net_expected_bps < config_.min_net_expected_bps) {
        return {OpportunityAction::Suppress, OpportunityReason::NegativeNetEdge};
    }

    // Правило 2: Conviction ниже порога → подавить
    if (conviction < eff_threshold) {
        return {OpportunityAction::Suppress, OpportunityReason::ConvictionBelowThreshold};
    }

    // Правило 3: Капитал исчерпан → подавить
    if (portfolio_ctx.gross_exposure_pct > config_.capital_exhaustion_threshold) {
        return {OpportunityAction::Suppress, OpportunityReason::CapitalExhausted};
    }

    // Правило 4: Высокая концентрация по символу → подавить
    if (portfolio_ctx.symbol_exposure_pct > config_.max_symbol_concentration) {
        return {OpportunityAction::Suppress, OpportunityReason::HighConcentration};
    }

    // Правило 5: Высокая концентрация по стратегии → defer
    if (portfolio_ctx.strategy_exposure_pct > config_.max_strategy_concentration) {
        return {OpportunityAction::Defer, OpportunityReason::HighConcentration};
    }

    // Правило 6: Высокая экспозиция при невысокой conviction → отложить
    if (portfolio_ctx.gross_exposure_pct > config_.high_exposure_threshold &&
        conviction < config_.high_exposure_min_conviction) {
        return {OpportunityAction::Defer, OpportunityReason::HighExposureLowConviction};
    }

    // Правило 7: Upgrade — кандидат лучше худшей позиции
    if (portfolio_ctx.has_worst_position &&
        score.net_expected_bps > portfolio_ctx.worst_position_net_bps +
                                  config_.upgrade_min_edge_advantage_bps) {
        return {OpportunityAction::Upgrade, OpportunityReason::UpgradeBetterCandidate};
    }

    // Правило 8: Достаточный чистый доход и капитал доступен → исполнить
    if (score.net_expected_bps > config_.execute_min_net_bps &&
        portfolio_ctx.gross_exposure_pct < config_.high_exposure_threshold) {
        return {OpportunityAction::Execute, OpportunityReason::StrongEdgeAvailable};
    }

    // Правило 9: Хороший conviction перевешивает высокую экспозицию,
    // но ТОЛЬКО если net edge положительный (правила 1-4 не сработали).
    // Это гарантирует, что высокий conviction не override'ит отрицательный ожидаемый доход.
    if (conviction >= config_.high_exposure_min_conviction &&
        score.net_expected_bps > 0.0) {
        return {OpportunityAction::Execute, OpportunityReason::HighConvictionOverride};
    }

    // Правило 10: Положительный, но недостаточный net edge → defer
    if (score.net_expected_bps > 0.0) {
        return {OpportunityAction::Defer, OpportunityReason::InsufficientNetEdge};
    }

    // По умолчанию → отложить
    return {OpportunityAction::Defer, OpportunityReason::DefaultDefer};
}

// ─── Построение аудит-факторов ───────────────────────────────────────────────

OpportunityCostFactors RuleBasedOpportunityCost::build_factors(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const PortfolioContext& portfolio_ctx,
    double conviction_threshold,
    const OpportunityScore& score,
    OpportunityAction action,
    OpportunityReason reason) const
{
    OpportunityCostFactors f;

    // Входные данные
    f.input_conviction = intent.conviction;
    f.input_urgency = intent.urgency;
    f.input_execution_cost_bps = exec_alpha.quality.total_cost_bps;
    f.input_exposure_pct = portfolio_ctx.gross_exposure_pct;
    f.input_conviction_threshold = effective_conviction_threshold(
        conviction_threshold, portfolio_ctx);

    // Правило, определившее решение
    f.reason = reason;
    switch (reason) {
        case OpportunityReason::NegativeNetEdge:           f.rule_id = 1; break;
        case OpportunityReason::ConvictionBelowThreshold:  f.rule_id = 2; break;
        case OpportunityReason::CapitalExhausted:          f.rule_id = 3; break;
        case OpportunityReason::HighConcentration:
            f.rule_id = (action == OpportunityAction::Suppress) ? 4 : 5; break;
        case OpportunityReason::HighExposureLowConviction: f.rule_id = 6; break;
        case OpportunityReason::UpgradeBetterCandidate:    f.rule_id = 7; break;
        case OpportunityReason::StrongEdgeAvailable:       f.rule_id = 8; break;
        case OpportunityReason::HighConvictionOverride:    f.rule_id = 9; break;
        case OpportunityReason::InsufficientNetEdge:       f.rule_id = 10; break;
        case OpportunityReason::DefaultDefer:              f.rule_id = 11; break;
        default: f.rule_id = 0; break;
    }

    // Counterfactual: что бы произошло при нулевой экспозиции?
    if (score.net_expected_bps >= config_.execute_min_net_bps) {
        f.would_be_without_exposure = OpportunityAction::Execute;
    } else if (score.net_expected_bps >= config_.min_net_expected_bps) {
        f.would_be_without_exposure = OpportunityAction::Defer;
    } else {
        f.would_be_without_exposure = OpportunityAction::Suppress;
    }

    // Portfolio flags
    f.concentration_limited =
        portfolio_ctx.symbol_exposure_pct > config_.max_symbol_concentration ||
        portfolio_ctx.strategy_exposure_pct > config_.max_strategy_concentration;
    f.capital_limited =
        portfolio_ctx.gross_exposure_pct > config_.capital_exhaustion_threshold;
    f.drawdown_penalized = portfolio_ctx.current_drawdown_pct > 0.0;

    return f;
}

// ─── Адаптивный conviction threshold ─────────────────────────────────────────

double RuleBasedOpportunityCost::effective_conviction_threshold(
    double base_threshold,
    const PortfolioContext& portfolio_ctx) const
{
    double threshold = base_threshold;

    double threshold_adj = 0.0;

    // Penalty за просадку: +drawdown_penalty_scale за каждые 5% drawdown от пика.
    // BUG-S16-06: NaN drawdown_pct / 0.05 = NaN → threshold = NaN → all checks disabled.
    if (std::isfinite(portfolio_ctx.current_drawdown_pct)
        && portfolio_ctx.current_drawdown_pct > 0.0) {
        threshold_adj += config_.drawdown_penalty_scale *
                         (portfolio_ctx.current_drawdown_pct / 0.05);
    }

    // Penalty за серию убытков: behavioral tilt protection.
    // BUG-S16-09: unbounded penalty (100 losses × 0.1 = 10.0) → threshold=0.95 after clamp
    // → only trades with conviction≥0.95 allowed → system can't recover.
    // Cap the loss-streak penalty to 0.20 to ensure recovery is still possible.
    if (portfolio_ctx.consecutive_losses > 0) {
        double loss_penalty = config_.consecutive_loss_penalty * portfolio_ctx.consecutive_losses;
        threshold_adj += std::min(loss_penalty, 0.20);
    }

    // Cap combined adjustment to prevent blocking all trading after moderate
    // drawdown or losing streaks.
    threshold_adj = std::min(threshold_adj, 0.20);

    threshold += threshold_adj;

    return std::clamp(threshold, 0.0, 0.95);
}

} // namespace tb::opportunity_cost
