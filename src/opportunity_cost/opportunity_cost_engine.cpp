#include "opportunity_cost/opportunity_cost_engine.hpp"
#include <algorithm>
#include <sstream>

namespace tb::opportunity_cost {

RuleBasedOpportunityCost::RuleBasedOpportunityCost(
    Config config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{
}

OpportunityCostResult RuleBasedOpportunityCost::evaluate(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    double current_exposure_pct,
    double conviction_threshold)
{
    OpportunityCostResult result;
    result.computed_at = clock_->now();
    result.budget_utilization = current_exposure_pct;

    // Рассчитать балл возможности
    result.score = compute_score(intent, exec_alpha, current_exposure_pct);

    // Определить действие
    result.action = determine_action(
        result.score, current_exposure_pct, intent.conviction, conviction_threshold);

    // Ранг по умолчанию (при одиночной оценке = 1)
    result.rank = 1;

    // Сформировать обоснование
    std::ostringstream oss;
    oss << "Действие=" << to_string(result.action)
        << " net_bps=" << result.score.net_expected_bps
        << " efficiency=" << result.score.capital_efficiency
        << " exposure=" << current_exposure_pct;
    result.rationale = oss.str();

    logger_->debug("OpportunityCost", "Оценка возможности",
                   {{"symbol", intent.symbol.get()},
                    {"action", to_string(result.action)},
                    {"net_bps", std::to_string(result.score.net_expected_bps)}});

    return result;
}

OpportunityScore RuleBasedOpportunityCost::compute_score(
    const strategy::TradeIntent& intent,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    double current_exposure_pct) const
{
    OpportunityScore score;

    // Ожидаемый доход: conviction отображается на ожидаемый доход
    // conviction 1.0 → 100 бп, conviction 0.5 → 50 бп
    score.expected_return_bps = intent.conviction * 100.0;

    // Стоимость исполнения из ExecutionAlpha
    score.execution_cost_bps = exec_alpha.quality.total_cost_bps;

    // Чистый ожидаемый доход
    score.net_expected_bps = score.expected_return_bps - score.execution_cost_bps;

    // Эффективность использования капитала
    // Чем выше чистый доход при меньшей экспозиции, тем выше эффективность
    score.capital_efficiency = score.net_expected_bps / (current_exposure_pct + 0.1);

    // Общий балл: нормализованный [0, 1]
    // Учитывает conviction, чистый доход и эффективность
    score.score = std::clamp(
        (intent.conviction * 0.4) +
        (std::clamp(score.net_expected_bps / 100.0, 0.0, 1.0) * 0.4) +
        (std::clamp(score.capital_efficiency / 100.0, 0.0, 1.0) * 0.2),
        0.0, 1.0);

    return score;
}

OpportunityAction RuleBasedOpportunityCost::determine_action(
    const OpportunityScore& score,
    double current_exposure_pct,
    double conviction,
    double conviction_threshold) const
{
    // Правило 1: Отрицательный чистый доход → подавить
    if (score.net_expected_bps < config_.min_net_expected_bps) {
        return OpportunityAction::Suppress;
    }

    // Правило 2: conviction ниже порога → подавить
    if (conviction < conviction_threshold) {
        return OpportunityAction::Suppress;
    }

    // Правило 3: Высокая экспозиция при невысокой conviction → отложить
    if (current_exposure_pct > config_.high_exposure_threshold &&
        conviction < config_.high_exposure_min_conviction) {
        return OpportunityAction::Defer;
    }

    // Правило 4: Достаточный чистый доход и капитал доступен → исполнить
    if (score.net_expected_bps > config_.execute_min_net_bps &&
        current_exposure_pct < config_.high_exposure_threshold) {
        return OpportunityAction::Execute;
    }

    // Правило 5: Хороший conviction, но высокая экспозиция → можно исполнить
    if (conviction >= config_.high_exposure_min_conviction) {
        return OpportunityAction::Execute;
    }

    // По умолчанию → отложить
    return OpportunityAction::Defer;
}

} // namespace tb::opportunity_cost
