#include "decision/decision_aggregation_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::decision {

// Forward declaration for ScoredIntent used in header
struct ScoredIntent {
    const strategy::TradeIntent* intent;
    double weighted_score;
    double weight;
    double aged_conviction;       // conviction after time decay
    double regime_conviction;     // conviction after regime adjustment
    double cost_penalty;          // execution cost penalty
};

CommitteeDecisionEngine::CommitteeDecisionEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    double conviction_threshold,
    double dominance_threshold,
    AdvancedDecisionConfig advanced)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , conviction_threshold_(conviction_threshold)
    , dominance_threshold_(dominance_threshold)
    , advanced_(std::move(advanced))
{}

DecisionRecord CommitteeDecisionEngine::aggregate(
    const Symbol& symbol,
    const std::vector<strategy::TradeIntent>& intents,
    const strategy_allocator::AllocationResult& allocation,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world,
    const uncertainty::UncertaintySnapshot& uncertainty,
    const std::optional<portfolio::PortfolioSnapshot>& portfolio,
    const std::optional<features::FeatureSnapshot>& features) {

    DecisionRecord record;
    record.symbol = symbol;
    record.decided_at = clock_->now();
    record.regime = regime.label;
    record.detailed_regime = regime.detailed;
    record.world_state = world.label;
    record.uncertainty = uncertainty.level;
    record.uncertainty_score = uncertainty.aggregate_score;

    std::ostringstream rationale;

    // ─── 0. Time-skew detection ─────────────────────────────────────────────

    if (advanced_.enable_time_skew_detection) {
        record.max_state_skew_ns = detect_time_skew(regime, uncertainty, record.decided_at);
        if (record.max_state_skew_ns > advanced_.max_state_skew_ns) {
            logger_->warn("Decision", "State time-skew detected",
                {{"symbol", symbol.get()},
                 {"skew_ms", std::to_string(record.max_state_skew_ns / 1'000'000)}});
        }
    }

    // ─── 1. Глобальные вето ─────────────────────────────────────────────────

    // Вето по неопределённости
    if (uncertainty.recommended_action == uncertainty::UncertaintyAction::NoTrade) {
        VetoReason veto;
        veto.source = "uncertainty";
        veto.reason = "Экстремальная неопределённость — торговля запрещена";
        veto.severity = 1.0;
        veto.reason_code = RejectionReason::GlobalUncertaintyVeto;
        record.global_vetoes.push_back(veto);
        rationale << "ВЕТО: " << veto.reason << ". ";
    }

    // Все стратегии отключены
    if (allocation.enabled_count == 0) {
        VetoReason veto;
        veto.source = "allocator";
        veto.reason = "Все стратегии отключены аллокатором";
        veto.severity = 1.0;
        veto.reason_code = RejectionReason::AllStrategiesDisabled;
        record.global_vetoes.push_back(veto);
        rationale << "ВЕТО: " << veto.reason << ". ";
    }

    // Если есть глобальные вето — отклоняем все
    if (!record.global_vetoes.empty()) {
        record.trade_approved = false;
        record.rejection_reason = record.global_vetoes.front().reason_code;
        record.rationale = rationale.str();
        for (const auto& intent : intents) {
            StrategyContribution contrib;
            contrib.strategy_id = intent.strategy_id;
            contrib.intent = intent;
            contrib.raw_conviction = intent.conviction;
            contrib.was_vetoed = true;
            contrib.veto_reasons = record.global_vetoes;
            record.contributions.push_back(std::move(contrib));
        }
        logger_->info("Decision", "Глобальное вето: " + record.rationale,
                      {{"symbol", symbol.get()},
                       {"reason_code", to_string(record.rejection_reason)}});
        return record;
    }

    // ─── 2. Execution cost estimation ───────────────────────────────────────

    ExecutionCostEstimate cost_estimate;
    if (advanced_.enable_execution_cost_modeling && features.has_value()) {
        cost_estimate = compute_execution_cost(*features);
        record.execution_cost = cost_estimate;

        if (cost_estimate.vetoed_by_cost) {
            VetoReason veto;
            veto.source = "execution_cost";
            veto.reason = "Стоимость исполнения слишком высока (" +
                std::to_string(static_cast<int>(cost_estimate.total_cost_bps)) + " bps)";
            veto.severity = 0.9;
            veto.reason_code = RejectionReason::ExecutionCostTooHigh;
            record.global_vetoes.push_back(veto);
            record.trade_approved = false;
            record.rejection_reason = RejectionReason::ExecutionCostTooHigh;
            record.rationale = veto.reason;
            for (const auto& intent : intents) {
                StrategyContribution contrib;
                contrib.strategy_id = intent.strategy_id;
                contrib.intent = intent;
                contrib.raw_conviction = intent.conviction;
                contrib.execution_cost_penalty = cost_estimate.conviction_penalty;
                contrib.was_vetoed = true;
                contrib.veto_reasons = record.global_vetoes;
                record.contributions.push_back(std::move(contrib));
            }
            logger_->info("Decision", "Вето: высокая стоимость исполнения",
                {{"symbol", symbol.get()},
                 {"cost_bps", std::to_string(cost_estimate.total_cost_bps)},
                 {"max_acceptable", std::to_string(advanced_.max_acceptable_cost_bps)}});
            return record;
        }
    }

    // ─── 3. Обработка каждого интента ───────────────────────────────────────

    auto find_allocation = [&](const StrategyId& sid) -> const strategy_allocator::StrategyAllocation* {
        for (const auto& a : allocation.allocations) {
            if (a.strategy_id == sid) {
                return &a;
            }
        }
        return nullptr;
    };

    std::vector<ScoredIntent> scored;
    bool has_buy = false;
    bool has_sell = false;

    for (const auto& intent : intents) {
        StrategyContribution contrib;
        contrib.strategy_id = intent.strategy_id;
        contrib.intent = intent;
        contrib.raw_conviction = intent.conviction;

        const auto* alloc = find_allocation(intent.strategy_id);
        if (!alloc || !alloc->is_enabled) {
            contrib.was_vetoed = true;
            contrib.veto_reasons.push_back(
                {"allocator", "Стратегия не включена в аллокацию", 1.0, RejectionReason::AllStrategiesDisabled});
            record.contributions.push_back(std::move(contrib));
            continue;
        }

        contrib.weight = alloc->weight;

        // Time decay: стахнувшие сигналы теряют conviction
        double aged_conviction = intent.conviction;
        if (advanced_.enable_time_decay) {
            if (intent.generated_at.get() > 0) {
                int64_t signal_age_ns = record.decided_at.get() - intent.generated_at.get();
                double decay = compute_time_decay(signal_age_ns);
                aged_conviction = intent.conviction * decay;
            } else {
                // Стратегия не установила generated_at — лог для диагностики.
                // Сигнал обрабатывается как свежий, но это признак ошибки в стратегии.
                logger_->warn("Decision", "TradeIntent без generated_at — time decay пропущен",
                    {{"strategy", intent.strategy_id.get()},
                     {"symbol", intent.symbol.get()}});
            }
        }
        contrib.aged_conviction = aged_conviction;

        // Execution cost penalty
        double effective_conviction = aged_conviction;
        contrib.execution_cost_penalty = cost_estimate.conviction_penalty;
        effective_conviction -= cost_estimate.conviction_penalty;
        effective_conviction = std::max(effective_conviction, 0.0);

        contrib.regime_adjusted_conviction = effective_conviction;

        double weighted_score = effective_conviction * alloc->weight;

        if (intent.side == Side::Buy) has_buy = true;
        if (intent.side == Side::Sell) has_sell = true;

        scored.push_back({&intent, weighted_score, alloc->weight,
                         aged_conviction, effective_conviction, cost_estimate.conviction_penalty});
        record.contributions.push_back(std::move(contrib));
    }

    // ─── 4. Разрешение конфликтов BUY/SELL ──────────────────────────────────

    if (has_buy && has_sell) {
        double buy_total = 0.0;
        double sell_total = 0.0;
        for (const auto& s : scored) {
            if (s.intent->side == Side::Buy) buy_total += s.weighted_score;
            else sell_total += s.weighted_score;
        }

        Side winning_side = (buy_total >= sell_total) ? Side::Buy : Side::Sell;
        double dominance = std::max(buy_total, sell_total) /
                          (buy_total + sell_total + 1e-10);

        // Regime-adaptive dominance threshold
        double effective_dominance_thr = dominance_threshold_;
        if (advanced_.enable_regime_dominance_scaling) {
            effective_dominance_thr = compute_regime_dominance_threshold(regime.detailed);
        }

        if (dominance < effective_dominance_thr) {
            VetoReason conflict_veto;
            conflict_veto.source = "conflict";
            conflict_veto.reason = "Конфликт сигналов без доминирующего направления (dominance=" +
                std::to_string(dominance) + " < " + std::to_string(effective_dominance_thr) + ")";
            conflict_veto.severity = 0.8;
            conflict_veto.reason_code = RejectionReason::SignalConflict;
            record.global_vetoes.push_back(conflict_veto);
            record.trade_approved = false;
            record.rejection_reason = RejectionReason::SignalConflict;
            record.rationale = conflict_veto.reason;
            for (auto& c : record.contributions) {
                c.was_vetoed = true;
                c.veto_reasons.push_back(conflict_veto);
            }
            logger_->debug("Decision", "Конфликт сигналов без доминирования",
                          {{"symbol", symbol.get()},
                           {"buy_score", std::to_string(buy_total)},
                           {"sell_score", std::to_string(sell_total)},
                           {"dominance", std::to_string(dominance)},
                           {"threshold", std::to_string(effective_dominance_thr)},
                           {"regime", regime::to_string(regime.detailed)}});
            return record;
        }

        // Убираем проигравшую сторону из кандидатов
        std::erase_if(scored, [winning_side](const auto& s) {
            return s.intent->side != winning_side;
        });

        logger_->debug("Decision", "Конфликт разрешён в пользу "
                       + std::string(winning_side == Side::Buy ? "BUY" : "SELL"),
                      {{"symbol", symbol.get()},
                       {"buy_score", std::to_string(buy_total)},
                       {"sell_score", std::to_string(sell_total)},
                       {"dominance", std::to_string(dominance)},
                       {"regime", regime::to_string(regime.detailed)}});
    }

    // ─── 5. Выбор лучшего интента ───────────────────────────────────────────

    if (scored.empty()) {
        record.trade_approved = false;
        record.rejection_reason = RejectionReason::NoValidIntents;
        record.rationale = "Нет подходящих торговых намерений";
        logger_->debug("Decision", record.rationale, {{"symbol", symbol.get()}});
        return record;
    }

    // Сортируем по weighted_score (убывание)
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.weighted_score > b.weighted_score; });

    const auto& best = scored.front();

    // ─── 6. Вычисление эффективного порога ──────────────────────────────────

    // 6a. Base threshold × uncertainty multiplier
    record.base_conviction_threshold = conviction_threshold_;
    double threshold = conviction_threshold_ * uncertainty.threshold_multiplier;

    // 6b. Regime-aware threshold scaling
    record.regime_threshold_factor = 1.0;
    if (advanced_.enable_regime_threshold_scaling) {
        record.regime_threshold_factor = compute_regime_threshold_factor(regime.detailed);
        threshold *= record.regime_threshold_factor;
    }

    // 6c. Drawdown-aware threshold boost
    record.drawdown_threshold_boost = 0.0;
    if (advanced_.enable_portfolio_awareness && portfolio.has_value()) {
        record.drawdown_threshold_boost = compute_drawdown_boost(*portfolio);
        threshold += record.drawdown_threshold_boost;
    }

    record.effective_threshold = threshold;

    // Используем aged conviction (после time decay + execution cost)
    double base_conviction = best.regime_conviction;

    // ─── 8. Ensemble conviction bonus ───────────────────────────────────────

    EnsembleMetrics ensemble;
    if (advanced_.enable_ensemble_conviction && scored.size() > 1) {
        ensemble = compute_ensemble_metrics(scored, best.intent->side);
    } else {
        ensemble.aligned_count = 1;
        ensemble.total_scored = static_cast<int>(scored.size());
        ensemble.agreement_ratio = scored.empty() ? 0.0 : 1.0 / scored.size();
        ensemble.leading_conviction = base_conviction;
        ensemble.ensemble_conviction = base_conviction;
    }
    record.ensemble = ensemble;

    double adjusted_conviction = ensemble.ensemble_conviction;
    // Clamp to [0, 1]
    adjusted_conviction = std::clamp(adjusted_conviction, 0.0, 1.0);

    // ─── 9. Порог проверка ──────────────────────────────────────────────────

    record.approval_gap = adjusted_conviction - threshold;

    if (adjusted_conviction < threshold) {
        record.trade_approved = false;
        record.rejection_reason = RejectionReason::LowConviction;
        rationale << "Лучший кандидат (" << best.intent->strategy_id.get()
                  << ") conviction=" << best.intent->conviction;
        if (best.aged_conviction != best.intent->conviction) {
            rationale << " aged=" << best.aged_conviction;
        }
        if (ensemble.ensemble_bonus > 0.0) {
            rationale << " (ensemble+" << ensemble.ensemble_bonus << ")";
        }
        rationale << " → effective=" << adjusted_conviction
                  << " < threshold=" << threshold;
        if (record.regime_threshold_factor != 1.0) {
            rationale << " (regime×" << record.regime_threshold_factor << ")";
        }
        if (record.drawdown_threshold_boost > 0.0) {
            rationale << " (dd+" << record.drawdown_threshold_boost << ")";
        }
        record.rationale = rationale.str();
        logger_->debug("Decision", record.rationale, {{"symbol", symbol.get()}});
        return record;
    }

    // ─── 10. Одобряем ───────────────────────────────────────────────────────

    record.trade_approved = true;
    record.rejection_reason = RejectionReason::None;
    record.final_intent = *best.intent;
    record.final_conviction = adjusted_conviction;
    record.correlation_id = best.intent->correlation_id;

    rationale << "Одобрено: " << best.intent->strategy_id.get()
              << " [" << best.intent->signal_name << "]"
              << " conviction=" << best.intent->conviction;
    if (best.aged_conviction != best.intent->conviction) {
        rationale << " aged=" << best.aged_conviction;
    }
    if (ensemble.ensemble_bonus > 0.0) {
        rationale << " (ensemble+" << ensemble.ensemble_bonus
                  << " aligned=" << ensemble.aligned_count << "/" << ensemble.total_scored << ")";
    }
    rationale << " → " << adjusted_conviction
              << " weight=" << best.weight
              << " threshold=" << threshold;
    if (cost_estimate.total_cost_bps > 0.0) {
        rationale << " exec_cost=" << cost_estimate.total_cost_bps << "bps";
    }
    record.rationale = rationale.str();

    logger_->debug("Decision", "Торговля одобрена: " + best.intent->strategy_id.get(),
                  {{"symbol", symbol.get()},
                   {"side", best.intent->side == Side::Buy ? "BUY" : "SELL"},
                   {"conviction", std::to_string(record.final_conviction)},
                   {"threshold", std::to_string(threshold)},
                   {"gap", std::to_string(record.approval_gap)},
                   {"ensemble_aligned", std::to_string(ensemble.aligned_count)},
                   {"regime", regime::to_string(regime.detailed)},
                   {"rejection", to_string(record.rejection_reason)}});

    return record;
}

// ─── Private helpers ────────────────────────────────────────────────────────

double CommitteeDecisionEngine::compute_regime_threshold_factor(
    regime::DetailedRegime r) const
{
    using DR = regime::DetailedRegime;
    switch (r) {
        case DR::StrongUptrend:
        case DR::StrongDowntrend:
            return advanced_.regime_trending_factor;

        case DR::WeakUptrend:
        case DR::WeakDowntrend:
            return advanced_.regime_trending_factor; // same as strong trend for micro-cap

        case DR::MeanReversion:
            return advanced_.regime_mean_reversion_factor;

        case DR::VolatilityExpansion:
            return advanced_.regime_volatile_factor;

        case DR::LowVolCompression:
            return 1.0; // micro-cap: compression is normal, don't inflate

        case DR::LiquidityStress:
        case DR::SpreadInstability:
        case DR::ToxicFlow:
            return advanced_.regime_stress_factor;

        case DR::AnomalyEvent:
            return 1.0; // anomaly — micro-cap markets are naturally noisy, don't inflate

        case DR::Chop:
            return advanced_.regime_choppy_factor;

        case DR::Undefined:
        default:
            return 1.0;
    }
}

double CommitteeDecisionEngine::compute_regime_dominance_threshold(
    regime::DetailedRegime r) const
{
    using DR = regime::DetailedRegime;
    switch (r) {
        case DR::StrongUptrend:
        case DR::StrongDowntrend:
            return advanced_.dominance_trending;

        case DR::WeakUptrend:
        case DR::WeakDowntrend:
            return (advanced_.dominance_trending + dominance_threshold_) / 2.0;

        case DR::VolatilityExpansion:
            return advanced_.dominance_volatile;

        case DR::Chop:
        case DR::MeanReversion:
            return advanced_.dominance_choppy;

        case DR::LiquidityStress:
        case DR::SpreadInstability:
        case DR::ToxicFlow:
        case DR::AnomalyEvent:
            return advanced_.dominance_stress;

        default:
            return dominance_threshold_;
    }
}

double CommitteeDecisionEngine::compute_time_decay(int64_t signal_age_ns) const {
    if (signal_age_ns <= 0) return 1.0;
    double age_ms = static_cast<double>(signal_age_ns) / 1'000'000.0;
    // λ = ln(2) / halflife → decay = exp(-λ × age)
    double lambda = 0.693147 / advanced_.time_decay_halflife_ms;
    return std::exp(-lambda * age_ms);
}

ExecutionCostEstimate CommitteeDecisionEngine::compute_execution_cost(
    const features::FeatureSnapshot& features) const
{
    ExecutionCostEstimate est;

    if (features.microstructure.spread_valid) {
        est.spread_bps = features.microstructure.spread_bps;
    }
    if (features.execution_context.slippage_valid) {
        est.estimated_slippage_bps = features.execution_context.estimated_slippage_bps;
    }

    est.total_cost_bps = est.spread_bps + est.estimated_slippage_bps;
    est.conviction_penalty = (est.total_cost_bps / 10000.0) * advanced_.execution_cost_conviction_penalty;

    if (est.total_cost_bps > advanced_.max_acceptable_cost_bps) {
        est.vetoed_by_cost = true;
    }

    return est;
}

double CommitteeDecisionEngine::compute_drawdown_boost(
    const portfolio::PortfolioSnapshot& portfolio) const
{
    double dd_pct = std::abs(portfolio.pnl.current_drawdown_pct);
    // Линейная шкала: за каждые 5% просадки +drawdown_boost_scale к порогу
    double boost = (dd_pct / 5.0) * advanced_.drawdown_boost_scale;

    // Бонус за серию убытков
    double loss_boost = portfolio.pnl.consecutive_losses * advanced_.consecutive_loss_boost;

    double total = boost + loss_boost;
    return std::min(total, advanced_.drawdown_max_boost);
}

EnsembleMetrics CommitteeDecisionEngine::compute_ensemble_metrics(
    const std::vector<ScoredIntent>& scored,
    Side winning_side) const
{
    EnsembleMetrics m;
    m.total_scored = static_cast<int>(scored.size());

    if (scored.empty()) return m;

    // Лидер — первый в sorted списке (max weighted_score)
    m.leading_conviction = scored.front().regime_conviction;

    // Подсчёт согласных стратегий (те же направления)
    double weighted_consensus = 0.0;
    double bonus = 0.0;
    double diminishing = 1.0;
    int aligned = 0;

    for (const auto& s : scored) {
        if (s.intent->side == winning_side) {
            ++aligned;
            weighted_consensus += s.regime_conviction * s.weight;

            // Бонус за каждую дополнительную согласную стратегию (после первой)
            if (aligned > 1) {
                bonus += advanced_.ensemble_agreement_bonus * diminishing;
                diminishing *= advanced_.ensemble_diminishing_factor;
            }
        }
    }

    m.aligned_count = aligned;
    m.agreement_ratio = m.total_scored > 0
        ? static_cast<double>(aligned) / m.total_scored
        : 0.0;
    m.weighted_consensus = weighted_consensus;
    m.ensemble_bonus = std::min(bonus, advanced_.ensemble_max_bonus);
    m.ensemble_conviction = m.leading_conviction + m.ensemble_bonus;

    return m;
}

int64_t CommitteeDecisionEngine::detect_time_skew(
    const regime::RegimeSnapshot& regime,
    const uncertainty::UncertaintySnapshot& uncertainty,
    Timestamp decided_at) const
{
    int64_t max_skew = 0;
    auto check = [&](int64_t computed_at) {
        if (computed_at > 0) {
            int64_t skew = std::abs(decided_at.get() - computed_at);
            max_skew = std::max(max_skew, skew);
        }
    };

    check(regime.computed_at.get());
    // UncertaintySnapshot может не иметь computed_at — проверяем через aggregate_score > 0
    // (если score посчитан, значит snap был создан)

    return max_skew;
}

} // namespace tb::decision
