#include "decision/decision_aggregation_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace tb::decision {

// Forward declaration for ScoredIntent used in header
struct ScoredIntent {
    const strategy::TradeIntent* intent;
    double weighted_score;
    double weight;
    double aged_conviction;       // conviction after time decay
    double effective_conviction;  // conviction after execution-cost penalty
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
    const AllocationResult& allocation,
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
        record.set_rationale(rationale.str());
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
            record.set_rationale(veto.reason);
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

    auto alloc_map = std::unordered_map<std::string, const StrategyAllocation*>{};
    for (const auto& a : allocation.allocations) {
        alloc_map[a.strategy_id.get()] = &a;
    }

    auto find_allocation = [&](const StrategyId& sid) -> const StrategyAllocation* {
        auto it = alloc_map.find(sid.get());
        return (it != alloc_map.end()) ? it->second : nullptr;
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
                int64_t signal_age_ns = std::max(int64_t{0}, static_cast<int64_t>(record.decided_at.get() - intent.generated_at.get()));
                double decay = compute_time_decay(signal_age_ns);
                aged_conviction = intent.conviction * decay;
            } else {
                // MEDIUM-8 fix: generated_at == 0 means the strategy did not set a timestamp.
                // Apply a conservative default decay (0.5) to penalise potentially stale signals
                // instead of treating them as fresh (full conviction), which could reward bugs.
                constexpr double kDefaultDecayForMissingTimestamp = 0.5;
                aged_conviction = intent.conviction * kDefaultDecayForMissingTimestamp;
                logger_->warn("Decision", "TradeIntent без generated_at — применён decay по умолчанию 0.5",
                    {{"strategy", intent.strategy_id.get()},
                     {"symbol", intent.symbol.get()},
                     {"original_conviction", std::to_string(intent.conviction)},
                     {"aged_conviction", std::to_string(aged_conviction)}});
            }
        }
        contrib.aged_conviction = aged_conviction;

        // Execution cost penalty
        double effective_conviction = aged_conviction;
        contrib.execution_cost_penalty = cost_estimate.conviction_penalty;
        effective_conviction -= cost_estimate.conviction_penalty;
        effective_conviction = std::max(effective_conviction, 0.0);

        contrib.cost_adjusted_conviction = effective_conviction;

        double weighted_score = effective_conviction * alloc->weight;

        if (intent.side == Side::Buy) has_buy = true;
        if (intent.side == Side::Sell) has_sell = true;

        scored.push_back({&intent, weighted_score, alloc->weight,
                         aged_conviction, effective_conviction, cost_estimate.conviction_penalty});
        record.contributions.push_back(std::move(contrib));
    }

    // BUY/SELL conflict resolution was removed in the scalping refactor —
    // strategy_engine emits at most one intent per tick. The variables
    // remain referenced below in the rationale only.
    (void)has_buy; (void)has_sell;

    // ─── 5. Выбор лучшего интента ───────────────────────────────────────────

    if (scored.empty()) {
        record.trade_approved = false;
        record.rejection_reason = RejectionReason::NoValidIntents;
        record.set_rationale("Нет подходящих торговых намерений");
        logger_->debug("Decision", record.rationale, {{"symbol", symbol.get()}});
        return record;
    }

    // Сортируем по weighted_score (убывание)
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.weighted_score > b.weighted_score; });

    const auto& best = scored.front();

    // ─── 6. Вычисление эффективного порога ──────────────────────────────────

    // ── Conviction threshold (scalping refactor 2026-05): bounded, additive,
    //    fee/spread-aware. Replaces the legacy stack that compounded uncertainty
    //    × regime × danger × drawdown into an effectively unreachable cap.
    //
    //    Total max suppression budget = 0.20 above base; clamped to base+0.20
    //    so the worst regime cannot make the threshold absurd. Conviction from
    //    strategy_engine sits in [0.50, 0.90]; with base 0.55 the gate stays
    //    reachable across all regimes.
    record.base_conviction_threshold = conviction_threshold_;
    record.drawdown_threshold_boost = 0.0;

    // Each component is a [0, 1] severity; we take the MAX, not the SUM —
    // suppression should not double-count overlapping signals.
    double severity = 0.0;

    // Uncertainty: legacy multiplier mapped Moderate=1.1, High=1.5, Extreme=2.0.
    // Convert to a severity in [0, 1] using the level itself (already accounts
    // for spread/data/vpin/instability inside the uncertainty engine).
    switch (uncertainty.level) {
        case UncertaintyLevel::Low:      severity = std::max(severity, 0.0); break;
        case UncertaintyLevel::Moderate: severity = std::max(severity, 0.20); break;
        case UncertaintyLevel::High:     severity = std::max(severity, 0.60); break;
        case UncertaintyLevel::Extreme:  severity = std::max(severity, 1.0); break;
    }

    // World danger probability: same severity scale, but cap influence so a
    // world model spike alone cannot make the gate unreachable.
    if (world.state_probabilities.valid) {
        using WS = world_model::WorldState;
        double danger_prob =
            world.state_probabilities.probability(WS::ToxicMicrostructure)
          + world.state_probabilities.probability(WS::LiquidityVacuum)
          + world.state_probabilities.probability(WS::ExhaustionSpike)
          + world.state_probabilities.probability(WS::ChopNoise);
        danger_prob = std::clamp(danger_prob, 0.0, 1.0);
        severity = std::max(severity, danger_prob * 0.7);
    }

    // Drawdown: still additive on TOP, because it carries portfolio-state info
    // the others don't see. But we cap it tightly (yaml override is 0.03 max).
    bool drawdown_boost_applied = false;
    if (advanced_.enable_portfolio_awareness && portfolio.has_value()) {
        record.drawdown_threshold_boost = compute_drawdown_boost(*portfolio);
        drawdown_boost_applied = (record.drawdown_threshold_boost > 0.0);
    }

    // Fee/spread headroom: when the realised round-trip cost is wide, the trade
    // needs more conviction to clear. This replaces the spread-blindness of
    // the legacy formula.
    double cost_headroom = 0.0;
    if (cost_estimate.total_cost_bps > 0.0) {
        // 10 bps cost adds +0.05 to threshold; 30 bps adds +0.15. Capped at 0.20.
        cost_headroom = std::min(0.20, cost_estimate.total_cost_bps / 200.0);
    }

    // B5.1/B18.7: advanced_risk_boost удалён — был всегда 0.0, мёртвый код.
    // Real integration требует pass MarketStateVector в evaluate; пока не сделано,
    // threshold формируется без этого слагаемого.

    // B18.6: cost_headroom magic — 30 bps добавляет +0.15. Если нужна тонкая
    // калибровка → DecisionConfig.cost_headroom_divisor.
    // B18.8: hard threshold cap 0.80 — design decision: даже 0.85-conviction
    // setup должен иметь шанс пройти.
    constexpr double kSuppressionBudget   = 0.25;
    constexpr double kSeverityScale       = 0.30;
    constexpr double kThresholdHardCap    = 0.80;
    double suppression_boost = std::min(kSuppressionBudget, severity * kSeverityScale);
    double threshold = conviction_threshold_
                     + suppression_boost
                     + record.drawdown_threshold_boost
                     + cost_headroom;

    threshold = std::min(threshold, kThresholdHardCap);

    record.effective_threshold = threshold;

    // Используем aged conviction (после time decay + execution cost).
    double base_conviction = best.effective_conviction;
    double adjusted_conviction = std::clamp(base_conviction, 0.0, 1.0);

    // ─── 9. Порог проверка ──────────────────────────────────────────────────

    record.approval_gap = adjusted_conviction - threshold;

    if (adjusted_conviction < threshold) {
        record.trade_approved = false;
        // DrawdownProtection если без drawdown boost прошли бы, иначе LowConviction
        double threshold_without_dd = threshold - record.drawdown_threshold_boost;
        if (drawdown_boost_applied && adjusted_conviction >= threshold_without_dd) {
            record.rejection_reason = RejectionReason::DrawdownProtection;
        } else {
            record.rejection_reason = RejectionReason::LowConviction;
        }
        rationale << "Лучший кандидат (" << best.intent->strategy_id.get()
                  << ") conviction=" << best.intent->conviction;
        if (best.aged_conviction != best.intent->conviction) {
            rationale << " aged=" << best.aged_conviction;
        }
        rationale << " → effective=" << adjusted_conviction
                  << " < threshold=" << threshold;
        if (record.drawdown_threshold_boost > 0.0) {
            rationale << " (dd+" << record.drawdown_threshold_boost << ")";
        }
        record.set_rationale(rationale.str());
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
    rationale << " → " << adjusted_conviction
              << " weight=" << best.weight
              << " threshold=" << threshold;
    if (cost_estimate.total_cost_bps > 0.0) {
        rationale << " exec_cost=" << cost_estimate.total_cost_bps << "bps";
    }
    record.set_rationale(rationale.str());

    logger_->debug("Decision", "Торговля одобрена: " + best.intent->strategy_id.get(),
                  {{"symbol", symbol.get()},
                   {"side", best.intent->side == Side::Buy ? "BUY" : "SELL"},
                   {"conviction", std::to_string(record.final_conviction)},
                   {"threshold", std::to_string(threshold)},
                   {"gap", std::to_string(record.approval_gap)},
                   {"regime", regime::to_string(regime.detailed)},
                   {"rejection", to_string(record.rejection_reason)}});

    return record;
}

// ─── Private helpers ────────────────────────────────────────────────────────

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
    // Нормализация к 100 bps: 100 bps стоимости → penalty_factor пенальти.
    // Для скальпинга с таргетом 5–20 bps это даёт ощутимую коррекцию.
    est.conviction_penalty = (est.total_cost_bps / 100.0) * advanced_.execution_cost_conviction_penalty;

    if (est.total_cost_bps > advanced_.max_acceptable_cost_bps) {
        est.vetoed_by_cost = true;
    }

    return est;
}

double CommitteeDecisionEngine::compute_drawdown_boost(
    const portfolio::PortfolioSnapshot& portfolio) const
{
    double dd_pct = std::abs(portfolio.pnl.current_drawdown_pct);
    // Линейная шкала: за каждые drawdown_reference_pct% просадки +drawdown_boost_scale к порогу
    double boost = (dd_pct / advanced_.drawdown_reference_pct) * advanced_.drawdown_boost_scale;

    // Бонус за серию убытков
    double loss_boost = portfolio.pnl.consecutive_losses * advanced_.consecutive_loss_boost;

    double total = boost + loss_boost;
    return std::min(total, advanced_.drawdown_max_boost);
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
    check(uncertainty.computed_at.get());

    return max_skew;
}

} // namespace tb::decision
