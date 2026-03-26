#include "decision/decision_aggregation_engine.hpp"
#include <algorithm>
#include <sstream>

namespace tb::decision {

CommitteeDecisionEngine::CommitteeDecisionEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    double conviction_threshold,
    double dominance_threshold)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , conviction_threshold_(conviction_threshold)
    , dominance_threshold_(dominance_threshold)
{}

DecisionRecord CommitteeDecisionEngine::aggregate(
    const Symbol& symbol,
    const std::vector<strategy::TradeIntent>& intents,
    const strategy_allocator::AllocationResult& allocation,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world,
    const uncertainty::UncertaintySnapshot& uncertainty,
    const std::optional<ai::AIAdvisoryResult>& ai_advisory) {

    DecisionRecord record;
    record.symbol = symbol;
    record.decided_at = clock_->now();
    record.regime = regime.label;
    record.world_state = world.label;
    record.uncertainty = uncertainty.level;
    record.uncertainty_score = uncertainty.aggregate_score;

    std::ostringstream rationale;

    // --- 1. Проверка глобальных вето ---

    // Вето по неопределённости
    if (uncertainty.recommended_action == uncertainty::UncertaintyAction::NoTrade) {
        VetoReason veto;
        veto.source = "uncertainty";
        veto.reason = "Экстремальная неопределённость — торговля запрещена";
        veto.severity = 1.0;
        record.global_vetoes.push_back(veto);
        rationale << "ВЕТО: " << veto.reason << ". ";
    }

    // Вето по качеству стакана (проверяем через любой интент, берём из allocation)
    // Если все стратегии отключены — это тоже вето
    if (allocation.enabled_count == 0) {
        VetoReason veto;
        veto.source = "allocator";
        veto.reason = "Все стратегии отключены аллокатором";
        veto.severity = 1.0;
        record.global_vetoes.push_back(veto);
        rationale << "ВЕТО: " << veto.reason << ". ";
    }

    // Если есть глобальные вето — отклоняем все
    if (!record.global_vetoes.empty()) {
        record.trade_approved = false;
        record.rationale = rationale.str();
        // Все интенты помечаем как vetoed
        for (const auto& intent : intents) {
            StrategyContribution contrib;
            contrib.strategy_id = intent.strategy_id;
            contrib.intent = intent;
            contrib.was_vetoed = true;
            contrib.veto_reasons = record.global_vetoes;
            record.contributions.push_back(std::move(contrib));
        }
        logger_->info("Decision", "Глобальное вето: " + record.rationale,
                      {{"symbol", symbol.get()}});
        return record;
    }

    // --- 1.5 AI Advisory processing ---
    if (ai_advisory.has_value() && !ai_advisory->empty()) {
        record.ai_advisories = ai_advisory->advisories;
        record.ai_confidence_adjustment = ai_advisory->total_confidence_adjustment;
        record.ai_veto_recommended = ai_advisory->has_veto;
        record.advisory_state = ai_advisory->advisory_state;
        record.advisory_size_multiplier = ai_advisory->advisory_size_multiplier;

        // Жёсткое вето — полный запрет торговли
        if (ai_advisory->advisory_state == ai::AdvisoryState::Veto) {
            VetoReason veto;
            veto.source = "ai_advisory";
            veto.reason = "AI Advisory: высокая серьёзность предупреждений (severity=" +
                std::to_string(ai_advisory->max_severity) + ")";
            veto.severity = ai_advisory->max_severity;
            record.global_vetoes.push_back(veto);
            rationale << "AI ВЕТО: " << ai_advisory->advisories.front().insight << ". ";

            record.trade_approved = false;
            record.rationale = rationale.str();
            for (const auto& intent : intents) {
                StrategyContribution contrib;
                contrib.strategy_id = intent.strategy_id;
                contrib.intent = intent;
                contrib.was_vetoed = true;
                contrib.veto_reasons = record.global_vetoes;
                record.contributions.push_back(std::move(contrib));
            }
            logger_->warn("Decision", "AI Advisory вето",
                {{"symbol", symbol.get()},
                 {"severity", std::to_string(ai_advisory->max_severity)},
                 {"advisories_count", std::to_string(ai_advisory->advisories.size())}});
            return record;
        }

        // Режим осторожности — торговля разрешена с уменьшенным размером позиции
        if (ai_advisory->advisory_state == ai::AdvisoryState::Caution) {
            rationale << "AI CAUTION: size_mult=" << ai_advisory->advisory_size_multiplier
                      << " severity=" << ai_advisory->max_severity << ". ";
            logger_->info("Decision", "AI Advisory режим осторожности",
                {{"symbol", symbol.get()},
                 {"severity", std::to_string(ai_advisory->max_severity)},
                 {"size_mult", std::to_string(ai_advisory->advisory_size_multiplier)},
                 {"ensemble_count", std::to_string(ai_advisory->ensemble_count)}});
        }
    }

    // --- 2. Обработка каждого интента ---

    // Маппинг аллокации по strategy_id
    auto find_allocation = [&](const StrategyId& sid) -> const strategy_allocator::StrategyAllocation* {
        for (const auto& a : allocation.allocations) {
            if (a.strategy_id == sid) {
                return &a;
            }
        }
        return nullptr;
    };

    struct ScoredIntent {
        const strategy::TradeIntent* intent;
        double weighted_score;
        double weight;
    };
    std::vector<ScoredIntent> scored;

    bool has_buy = false;
    bool has_sell = false;

    for (const auto& intent : intents) {
        StrategyContribution contrib;
        contrib.strategy_id = intent.strategy_id;
        contrib.intent = intent;

        const auto* alloc = find_allocation(intent.strategy_id);
        if (!alloc || !alloc->is_enabled) {
            contrib.was_vetoed = true;
            contrib.veto_reasons.push_back(
                {"allocator", "Стратегия не включена в аллокацию", 1.0});
            record.contributions.push_back(std::move(contrib));
            continue;
        }

        contrib.weight = alloc->weight;
        double weighted_score = intent.conviction * alloc->weight;

        // Трекинг направлений для обнаружения конфликтов
        if (intent.side == Side::Buy) has_buy = true;
        if (intent.side == Side::Sell) has_sell = true;

        scored.push_back({&intent, weighted_score, alloc->weight});
        record.contributions.push_back(std::move(contrib));
    }

    // --- 3. Разрешение конфликтов BUY/SELL ---
    // При конфликте направлений оставляем сторону с бОльшим суммарным
    // weighted_score. Это позволяет доминирующему сигналу пройти,
    // когда слабый сигнал противоположного направления не значим.
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

        // Если нет явного доминирования (< 60%) — вето, рынок неопределён
        if (dominance < dominance_threshold_) {
            VetoReason conflict_veto;
            conflict_veto.source = "conflict";
            conflict_veto.reason = "Конфликт сигналов без доминирующего направления";
            conflict_veto.severity = 0.8;
            record.global_vetoes.push_back(conflict_veto);
            record.trade_approved = false;
            record.rationale = conflict_veto.reason;
            for (auto& c : record.contributions) {
                c.was_vetoed = true;
                c.veto_reasons.push_back(conflict_veto);
            }
            logger_->debug("Decision", "Конфликт сигналов без доминирования",
                          {{"symbol", symbol.get()},
                           {"buy_score", std::to_string(buy_total)},
                           {"sell_score", std::to_string(sell_total)}});
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
                       {"dominance", std::to_string(dominance)}});
    }

    // --- 4. Выбор лучшего интента ---
    if (scored.empty()) {
        record.trade_approved = false;
        record.rationale = "Нет подходящих торговых намерений";
        logger_->debug("Decision", record.rationale, {{"symbol", symbol.get()}});
        return record;
    }

    // Сортируем по weighted_score (убывание)
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.weighted_score > b.weighted_score; });

    const auto& best = scored.front();

    // --- 5. Проверка порога ---
    // Порог сравнивается с conviction (не weighted_score), потому что
    // weighted_score уже нормализован на число стратегий и становится
    // слишком малым при 4-5 активных стратегиях.
    // Weight используется только для ранжирования (кто из кандидатов лучший).
    double threshold = conviction_threshold_ * uncertainty.threshold_multiplier;

    // AI Advisory confidence adjustment: корректируем conviction лучшего кандидата
    double ai_adj = 0.0;
    if (ai_advisory.has_value()) {
        ai_adj = ai_advisory->total_confidence_adjustment;
    }
    double adjusted_conviction = best.intent->conviction + ai_adj;

    if (adjusted_conviction < threshold) {
        record.trade_approved = false;
        rationale << "Лучший кандидат (" << best.intent->strategy_id.get()
                  << ") с conviction=" << best.intent->conviction;
        if (ai_adj != 0.0) {
            rationale << " (AI adj=" << ai_adj << " → " << adjusted_conviction << ")";
        }
        rationale << " ниже порога=" << threshold;
        record.rationale = rationale.str();
        logger_->debug("Decision", record.rationale, {{"symbol", symbol.get()}});
        return record;
    }

    // --- 6. Одобряем ---
    record.trade_approved = true;
    record.final_intent = *best.intent;
    // final_conviction хранит conviction стратегии (не weighted_score),
    // чтобы downstream consumers получали реальную уверенность [0,1]
    record.final_conviction = adjusted_conviction;
    record.correlation_id = best.intent->correlation_id;

    rationale << "Одобрено: " << best.intent->strategy_id.get()
              << " [" << best.intent->signal_name << "]"
              << " conviction=" << best.intent->conviction;
    if (ai_adj != 0.0) {
        rationale << " (AI adj=" << ai_adj << " → " << adjusted_conviction << ")";
    }
    rationale << " weight=" << best.weight
              << " weighted_score=" << best.weighted_score
              << " threshold=" << threshold;
    record.rationale = rationale.str();

    logger_->debug("Decision", "Торговля одобрена: " + best.intent->strategy_id.get(),
                  {{"symbol", symbol.get()},
                   {"side", best.intent->side == Side::Buy ? "BUY" : "SELL"},
                   {"conviction", std::to_string(record.final_conviction)}});

    return record;
}

} // namespace tb::decision
