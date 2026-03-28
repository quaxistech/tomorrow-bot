#include "world_model/world_model_engine.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace tb::world_model {

// ============================================================
// WorldModelConfig
// ============================================================

WorldModelConfig WorldModelConfig::make_default() {
    WorldModelConfig cfg;
    cfg.suitability = SuitabilityConfig::make_default();
    return cfg;
}

bool WorldModelConfig::validate() const {
    if (stable_trend.adx_min <= 0.0 || stable_trend.adx_min > 100.0) return false;
    if (chop_noise.adx_max <= 0.0 || chop_noise.adx_max > 100.0) return false;
    if (exhaustion.rsi_upper <= exhaustion.rsi_lower) return false;
    if (liquidity_vacuum.spread_bps_critical <= 0.0) return false;
    if (hysteresis.confirmation_ticks < 0) return false;
    if (history.max_entries == 0) return false;
    return true;
}

// ============================================================
// SuitabilityConfig
// ============================================================

SuitabilityConfig SuitabilityConfig::make_default() {
    SuitabilityConfig cfg;

    auto add = [](std::vector<SuitabilityEntry>& v,
                  const std::string& id, double suit, const std::string& reason) {
        v.push_back({id, suit, reason});
    };

    // StableTrendContinuation (0)
    {
        auto& v = cfg.table[0];
        add(v, "momentum",             0.9, "Устойчивый тренд — идеально для momentum");
        add(v, "mean_reversion",       0.2, "Тренд — не подходит для возврата к среднему");
        add(v, "breakout",             0.4, "Тренд уже установлен, пробой менее вероятен");
        add(v, "microstructure_scalp", 0.5, "Стабильная микроструктура допускает скальпинг");
        add(v, "vol_expansion",        0.3, "Волатильность стабильна, расширение маловероятно");
    }
    // FragileBreakout (1)
    {
        auto& v = cfg.table[1];
        add(v, "momentum",             0.5, "Пробой может перерасти в тренд");
        add(v, "mean_reversion",       0.1, "Пробой — опасно торговать против");
        add(v, "breakout",             0.8, "Пробой — основной сценарий");
        add(v, "microstructure_scalp", 0.3, "Хрупкая микроструктура");
        add(v, "vol_expansion",        0.7, "Волатильность расширяется");
    }
    // CompressionBeforeExpansion (2)
    {
        auto& v = cfg.table[2];
        add(v, "momentum",             0.2, "Ещё нет направления");
        add(v, "mean_reversion",       0.6, "Можно торговать в диапазоне до пробоя");
        add(v, "breakout",             0.9, "Сжатие — лучший сетап для пробоя");
        add(v, "microstructure_scalp", 0.6, "Узкие спреды в сжатии");
        add(v, "vol_expansion",        0.8, "Ожидание расширения волатильности");
    }
    // ChopNoise (3)
    {
        auto& v = cfg.table[3];
        add(v, "momentum",             0.1, "Нет тренда — momentum неэффективен");
        add(v, "mean_reversion",       0.7, "Боковик — хорошо для mean reversion");
        add(v, "breakout",             0.1, "Ложные пробои в боковике");
        add(v, "microstructure_scalp", 0.8, "Стабильный стакан — хорошо для скальпинга");
        add(v, "vol_expansion",        0.1, "Низкая волатильность, расширение маловероятно");
    }
    // ExhaustionSpike (4)
    {
        auto& v = cfg.table[4];
        add(v, "momentum",             0.1, "Импульс истощён");
        add(v, "mean_reversion",       0.6, "Возврат после спайка вероятен");
        add(v, "breakout",             0.1, "Ложный сигнал пробоя");
        add(v, "microstructure_scalp", 0.1, "Нестабильная микроструктура");
        add(v, "vol_expansion",        0.3, "Волатильность уже высока");
    }
    // LiquidityVacuum (5)
    {
        auto& v = cfg.table[5];
        add(v, "momentum",             0.05, "Вакуум ликвидности — торговля опасна");
        add(v, "mean_reversion",       0.05, "Вакуум ликвидности — торговля опасна");
        add(v, "breakout",             0.05, "Вакуум ликвидности — торговля опасна");
        add(v, "microstructure_scalp", 0.0,  "Вакуум ликвидности — скальпинг невозможен");
        add(v, "vol_expansion",        0.05, "Вакуум ликвидности — торговля опасна");
    }
    // ToxicMicrostructure (6)
    {
        auto& v = cfg.table[6];
        add(v, "momentum",             0.1, "Токсичный поток искажает сигналы");
        add(v, "mean_reversion",       0.1, "Токсичная микроструктура");
        add(v, "breakout",             0.1, "Манипулятивные пробои");
        add(v, "microstructure_scalp", 0.0, "Микроструктура токсична — скальпинг запрещён");
        add(v, "vol_expansion",        0.2, "Возможна повышенная волатильность");
    }
    // PostShockStabilization (7)
    {
        auto& v = cfg.table[7];
        add(v, "momentum",             0.3, "Направление после шока неясно");
        add(v, "mean_reversion",       0.5, "Возврат к нормальным уровням");
        add(v, "breakout",             0.2, "Повторный шок маловероятен");
        add(v, "microstructure_scalp", 0.4, "Микроструктура стабилизируется");
        add(v, "vol_expansion",        0.4, "Волатильность снижается");
    }
    // Unknown (8)
    {
        auto& v = cfg.table[8];
        add(v, "momentum",             0.3, "Состояние неизвестно — пониженный вес");
        add(v, "mean_reversion",       0.3, "Состояние неизвестно — пониженный вес");
        add(v, "breakout",             0.3, "Состояние неизвестно — пониженный вес");
        add(v, "microstructure_scalp", 0.3, "Состояние неизвестно — пониженный вес");
        add(v, "vol_expansion",        0.3, "Состояние неизвестно — пониженный вес");
    }

    return cfg;
}

// ============================================================
// SymbolContext (history)
// ============================================================

void SymbolContext::push_history(const HistoryEntry& entry, size_t max_size) {
    history.push_back(entry);
    while (history.size() > max_size) {
        history.pop_front();
    }
}

void SymbolContext::record_transition(WorldState from, WorldState to) {
    int fi = static_cast<int>(from);
    int ti = static_cast<int>(to);
    if (fi >= 0 && fi < static_cast<int>(kNumStates) &&
        ti >= 0 && ti < static_cast<int>(kNumStates)) {
        ++transition_counts[fi][ti];
        ++total_transitions;
    }
    int si = static_cast<int>(to);
    if (si >= 0 && si < static_cast<int>(kNumStates)) {
        ++state_total_count[si];
        if (from == to) ++state_stay_count[si];
    }
}

double SymbolContext::empirical_persistence(WorldState state) const {
    int idx = static_cast<int>(state);
    if (idx < 0 || idx >= static_cast<int>(kNumStates)) return 0.5;
    if (state_total_count[idx] == 0) return 0.5;
    return static_cast<double>(state_stay_count[idx]) / state_total_count[idx];
}

double SymbolContext::transition_probability(WorldState from, WorldState to) const {
    int fi = static_cast<int>(from);
    int ti = static_cast<int>(to);
    if (fi < 0 || fi >= static_cast<int>(kNumStates)) return 0.0;
    int row_sum = 0;
    for (size_t j = 0; j < kNumStates; ++j) {
        row_sum += transition_counts[fi][j];
    }
    if (row_sum == 0) return 0.0;
    return static_cast<double>(transition_counts[fi][ti]) / row_sum;
}

double SymbolContext::recent_avg_fragility(size_t n) const {
    if (history.empty()) return 0.5;
    size_t count = std::min(n, history.size());
    double sum = 0.0;
    for (auto it = history.rbegin(); count > 0 && it != history.rend(); ++it, --count) {
        sum += it->fragility;
    }
    return sum / std::min(n, history.size());
}

int SymbolContext::recent_transition_count(size_t n) const {
    if (history.size() < 2) return 0;
    int transitions = 0;
    size_t count = std::min(n, history.size());
    auto it = history.rbegin();
    WorldState prev = it->state;
    ++it;
    for (size_t i = 1; i < count && it != history.rend(); ++i, ++it) {
        if (it->state != prev) ++transitions;
        prev = it->state;
    }
    return transitions;
}

// ============================================================
// Преобразования в строку
// ============================================================

std::string to_string(WorldState state) {
    switch (state) {
        case WorldState::StableTrendContinuation:    return "StableTrendContinuation";
        case WorldState::FragileBreakout:            return "FragileBreakout";
        case WorldState::CompressionBeforeExpansion: return "CompressionBeforeExpansion";
        case WorldState::ChopNoise:                  return "ChopNoise";
        case WorldState::ExhaustionSpike:            return "ExhaustionSpike";
        case WorldState::LiquidityVacuum:            return "LiquidityVacuum";
        case WorldState::ToxicMicrostructure:        return "ToxicMicrostructure";
        case WorldState::PostShockStabilization:     return "PostShockStabilization";
        case WorldState::Unknown:                    return "Unknown";
    }
    return "Unknown";
}

std::string to_string(TransitionTendency tendency) {
    switch (tendency) {
        case TransitionTendency::Stable:        return "Stable";
        case TransitionTendency::Improving:     return "Improving";
        case TransitionTendency::Deteriorating: return "Deteriorating";
        case TransitionTendency::Ambiguous:     return "Ambiguous";
    }
    return "Ambiguous";
}

// ============================================================
// WorldModelSnapshot
// ============================================================

WorldStateLabel WorldModelSnapshot::to_label(WorldState s) {
    switch (s) {
        case WorldState::StableTrendContinuation:
            return WorldStateLabel::Stable;
        case WorldState::FragileBreakout:
        case WorldState::CompressionBeforeExpansion:
        case WorldState::PostShockStabilization:
        case WorldState::ChopNoise:
            return WorldStateLabel::Transitioning;
        case WorldState::ExhaustionSpike:
        case WorldState::LiquidityVacuum:
        case WorldState::ToxicMicrostructure:
            return WorldStateLabel::Disrupted;
        case WorldState::Unknown:
            return WorldStateLabel::Unknown;
    }
    return WorldStateLabel::Unknown;
}

// ============================================================
// RuleBasedWorldModelEngine — конструкторы
// ============================================================

RuleBasedWorldModelEngine::RuleBasedWorldModelEngine(
    WorldModelConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

RuleBasedWorldModelEngine::RuleBasedWorldModelEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(WorldModelConfig::make_default())
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

// ============================================================
// update() — основной метод
// ============================================================

WorldModelSnapshot RuleBasedWorldModelEngine::update(const features::FeatureSnapshot& snapshot) {
    const auto& sym = snapshot.symbol.get();
    auto now = clock_->now();

    std::lock_guard lock(mutex_);

    // Получаем или создаём per-symbol контекст
    auto& ctx = contexts_[sym];

    // 1. Непосредственная классификация (до гистерезиса)
    WorldModelExplanation explanation;
    auto immediate = classify_immediate(snapshot, ctx, explanation);
    explanation.immediate_state = immediate;

    // 2. Гистерезис
    auto confirmed = apply_hysteresis(immediate, ctx, explanation);
    explanation.confirmed_state = confirmed;

    // 3. Регистрируем переход (если был)
    WorldState previous = ctx.confirmed_snapshot.state;
    ctx.record_transition(previous, confirmed);

    // 4. Вычисляем метрики
    auto fragility = compute_fragility(snapshot, confirmed, ctx);
    double persistence = compute_persistence(confirmed, ctx);
    auto transition = compute_transition(confirmed, ctx);
    double confidence = compute_confidence(snapshot, confirmed, ctx, explanation);
    auto state_probs = compute_state_probabilities(confirmed, confidence, ctx);
    auto suitability = compute_suitability(confirmed, snapshot, ctx);

    // 5. Explainability
    explanation.top_drivers = compute_top_drivers(snapshot, confirmed);
    explanation.data_quality_score = assess_data_quality(snapshot);
    explanation.summary = generate_summary(explanation, fragility, confidence);

    // 6. Собираем снимок
    WorldModelSnapshot result;
    // v1 поля
    result.state = confirmed;
    result.label = WorldModelSnapshot::to_label(confirmed);
    result.fragility = fragility;
    result.tendency = transition.tendency;
    result.persistence_score = persistence;
    result.strategy_suitability = std::move(suitability);
    result.computed_at = now;
    result.symbol = snapshot.symbol;
    // v2 поля
    result.confidence = confidence;
    result.state_probabilities = state_probs;
    result.transition = transition;
    result.explanation = std::move(explanation);
    result.model_version = config_.model_version;
    result.dwell_ticks = ctx.dwell_ticks;

    // 7. Обновляем историю
    HistoryEntry entry;
    entry.state = confirmed;
    entry.fragility = fragility.value;
    entry.confidence = confidence;
    entry.timestamp = now;
    ctx.push_history(entry, config_.history.max_entries);

    // 8. Обновляем dwell
    if (confirmed == previous || previous == WorldState::Unknown) {
        ++ctx.dwell_ticks;
    } else {
        ctx.dwell_ticks = 1;
    }

    // 9. Сохраняем снимок
    ctx.confirmed_snapshot = result;

    // 10. Логируем
    bool state_changed = (confirmed != previous && previous != WorldState::Unknown);
    if (state_changed) {
        logger_->info("WorldModel",
            "Смена состояния: " + to_string(previous) + " → " + to_string(confirmed),
            {{"symbol", sym},
             {"from", to_string(previous)},
             {"to", to_string(confirmed)},
             {"confidence", std::to_string(confidence)},
             {"fragility", std::to_string(fragility.value)},
             {"dwell_before", std::to_string(ctx.dwell_ticks)},
             {"model_version", config_.model_version}});
    } else {
        logger_->debug("WorldModel",
            "Состояние: " + to_string(confirmed) + " (dwell=" + std::to_string(ctx.dwell_ticks) + ")",
            {{"symbol", sym},
             {"state", to_string(confirmed)},
             {"confidence", std::to_string(confidence)},
             {"fragility", std::to_string(fragility.value)},
             {"persistence", std::to_string(persistence)},
             {"tendency", to_string(transition.tendency)}});
    }

    return result;
}

// ============================================================
// current_state()
// ============================================================

std::optional<WorldModelSnapshot> RuleBasedWorldModelEngine::current_state(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = contexts_.find(symbol.get());
    if (it != contexts_.end() && it->second.confirmed_snapshot.state != WorldState::Unknown) {
        return it->second.confirmed_snapshot;
    }
    // Совместимость: возвращаем даже Unknown если контекст создан
    if (it != contexts_.end()) {
        return it->second.confirmed_snapshot;
    }
    return std::nullopt;
}

std::string RuleBasedWorldModelEngine::model_version() const {
    return config_.model_version;
}

// ============================================================
// classify_immediate() — чистая классификация без гистерезиса
// ============================================================

WorldState RuleBasedWorldModelEngine::classify_immediate(
    const features::FeatureSnapshot& snap,
    const SymbolContext& ctx,
    WorldModelExplanation& explanation) const {

    const auto& tech = snap.technical;
    const auto& micro = snap.microstructure;

    // Подсчёт валидных индикаторов
    int valid = 0;
    int total = 7; // sma, rsi, bb, atr, adx, volatility, momentum + spread, instability, flow, liquidity, vpin
    total = 12;
    if (tech.sma_valid)        ++valid;
    if (tech.rsi_valid)        ++valid;
    if (tech.bb_valid)         ++valid;
    if (tech.atr_valid)        ++valid;
    if (tech.adx_valid)        ++valid;
    if (tech.volatility_valid) ++valid;
    if (tech.momentum_valid)   ++valid;
    if (micro.spread_valid)    ++valid;
    if (micro.instability_valid) ++valid;
    if (micro.trade_flow_valid)  ++valid;
    if (micro.liquidity_valid)   ++valid;
    if (micro.vpin_valid)        ++valid;

    explanation.valid_indicator_count = valid;
    explanation.total_indicator_count = total;

    // Недостаточно данных
    if (valid < config_.min_valid_indicators) {
        ClassificationCondition cond;
        cond.rule_name = "InsufficientData";
        cond.triggered = true;
        cond.detail = "valid=" + std::to_string(valid) + " < min=" + std::to_string(config_.min_valid_indicators);
        explanation.triggered_conditions.push_back(cond);
        return WorldState::Unknown;
    }

    // ── Вспомогательная лямбда для записи проверки ───────────
    auto check = [&](const std::string& name, bool triggered, double proximity,
                     const std::string& detail) {
        ClassificationCondition cond;
        cond.rule_name = name;
        cond.triggered = triggered;
        cond.proximity = proximity;
        cond.detail = detail;
        if (triggered) {
            explanation.triggered_conditions.push_back(cond);
        }
        explanation.checked_conditions.push_back(cond);
        return triggered;
    };

    // ── PostShockStabilization ───────────────────────────────
    {
        auto prev = ctx.confirmed_snapshot.state;
        bool was_shock = (prev == WorldState::ExhaustionSpike || prev == WorldState::LiquidityVacuum);
        bool vol_recovering = tech.volatility_valid && tech.volatility_5 < tech.volatility_20;

        double prox = 0.0;
        if (was_shock && tech.volatility_valid) {
            prox = tech.volatility_20 > 0.0
                   ? 1.0 - (tech.volatility_5 / tech.volatility_20)
                   : 0.0;
            prox = std::clamp(prox, 0.0, 1.0);
        }

        if (check("PostShockStabilization", was_shock && vol_recovering, prox,
                   "prev=" + to_string(prev) + " vol5=" +
                   std::to_string(tech.volatility_valid ? tech.volatility_5 : -1.0) +
                   " vol20=" + std::to_string(tech.volatility_valid ? tech.volatility_20 : -1.0))) {
            return WorldState::PostShockStabilization;
        }
    }

    // ── ToxicMicrostructure ──────────────────────────────────
    {
        bool instability_ok = micro.instability_valid &&
                              micro.book_instability > config_.toxic.book_instability_min;
        bool flow_ok = micro.trade_flow_valid &&
                       micro.aggressive_flow > config_.toxic.aggressive_flow_min;
        bool spread_ok = micro.spread_valid &&
                         micro.spread_bps > config_.toxic.spread_bps_min;
        bool triggered = instability_ok && flow_ok && spread_ok;

        double prox = 0.0;
        if (micro.instability_valid && micro.trade_flow_valid && micro.spread_valid) {
            double p1 = micro.book_instability / config_.toxic.book_instability_min;
            double p2 = micro.aggressive_flow / config_.toxic.aggressive_flow_min;
            double p3 = micro.spread_bps / config_.toxic.spread_bps_min;
            prox = std::min({p1, p2, p3});
            prox = std::clamp(prox, 0.0, 1.0);
        }

        std::string detail = "instability=" + std::to_string(micro.instability_valid ? micro.book_instability : -1.0) +
                             " flow=" + std::to_string(micro.trade_flow_valid ? micro.aggressive_flow : -1.0) +
                             " spread=" + std::to_string(micro.spread_valid ? micro.spread_bps : -1.0);

        if (check("ToxicMicrostructure", triggered, prox, detail)) {
            return WorldState::ToxicMicrostructure;
        }
    }

    // ── LiquidityVacuum (критический спред) ──────────────────
    {
        bool critical_spread = micro.spread_valid &&
                               micro.spread_bps > config_.liquidity_vacuum.spread_bps_critical;
        double prox1 = micro.spread_valid
                       ? micro.spread_bps / config_.liquidity_vacuum.spread_bps_critical
                       : 0.0;

        if (check("LiquidityVacuum_CriticalSpread", critical_spread,
                   std::clamp(prox1, 0.0, 1.0),
                   "spread_bps=" + std::to_string(micro.spread_valid ? micro.spread_bps : -1.0))) {
            return WorldState::LiquidityVacuum;
        }
    }

    // ── LiquidityVacuum (перекос ликвидности) ────────────────
    {
        bool liquidity_skew = micro.liquidity_valid &&
                              micro.liquidity_ratio < config_.liquidity_vacuum.liquidity_ratio_min &&
                              micro.spread_valid &&
                              micro.spread_bps > config_.liquidity_vacuum.spread_bps_secondary;
        double prox = 0.0;
        if (micro.liquidity_valid && micro.spread_valid) {
            double p1 = 1.0 - (micro.liquidity_ratio / config_.liquidity_vacuum.liquidity_ratio_min);
            double p2 = micro.spread_bps / config_.liquidity_vacuum.spread_bps_secondary;
            prox = std::clamp(std::min(p1, p2), 0.0, 1.0);
        }

        if (check("LiquidityVacuum_Skew", liquidity_skew, prox,
                   "liq_ratio=" + std::to_string(micro.liquidity_valid ? micro.liquidity_ratio : -1.0) +
                   " spread=" + std::to_string(micro.spread_valid ? micro.spread_bps : -1.0))) {
            return WorldState::LiquidityVacuum;
        }
    }

    // ── ExhaustionSpike ──────────────────────────────────────
    {
        bool rsi_extreme = tech.rsi_valid &&
                           (tech.rsi_14 > config_.exhaustion.rsi_upper ||
                            tech.rsi_14 < config_.exhaustion.rsi_lower);
        bool high_momentum = tech.momentum_valid &&
                             std::abs(tech.momentum_5) > config_.exhaustion.momentum_abs_min;
        bool triggered = rsi_extreme && high_momentum;

        double prox = 0.0;
        if (tech.rsi_valid && tech.momentum_valid) {
            double rsi_dist = std::max(
                tech.rsi_14 / config_.exhaustion.rsi_upper,
                config_.exhaustion.rsi_lower / std::max(tech.rsi_14, 0.01));
            double mom_ratio = std::abs(tech.momentum_5) / config_.exhaustion.momentum_abs_min;
            prox = std::clamp(std::min(rsi_dist, mom_ratio), 0.0, 1.0);
        }

        if (check("ExhaustionSpike", triggered, prox,
                   "rsi=" + std::to_string(tech.rsi_valid ? tech.rsi_14 : -1.0) +
                   " momentum=" + std::to_string(tech.momentum_valid ? tech.momentum_5 : -1.0))) {
            return WorldState::ExhaustionSpike;
        }
    }

    // ── FragileBreakout ──────────────────────────────────────
    {
        bool bb_edge = tech.bb_valid &&
                       (tech.bb_percent_b > config_.fragile_breakout.bb_percent_b_upper ||
                        tech.bb_percent_b < config_.fragile_breakout.bb_percent_b_lower);
        bool high_vol = tech.volatility_valid &&
                        tech.volatility_5 > config_.fragile_breakout.volatility_5_min;
        bool book_skew = micro.book_imbalance_valid &&
                         std::abs(micro.book_imbalance_5) > config_.fragile_breakout.book_imbalance_abs_min;
        bool triggered = bb_edge && high_vol && book_skew;

        double prox = 0.0;
        if (tech.bb_valid && tech.volatility_valid && micro.book_imbalance_valid) {
            double p1 = std::max(tech.bb_percent_b / config_.fragile_breakout.bb_percent_b_upper,
                                 config_.fragile_breakout.bb_percent_b_lower / std::max(tech.bb_percent_b, 0.001));
            double p2 = tech.volatility_5 / config_.fragile_breakout.volatility_5_min;
            double p3 = std::abs(micro.book_imbalance_5) / config_.fragile_breakout.book_imbalance_abs_min;
            prox = std::clamp(std::min({p1, p2, p3}), 0.0, 1.0);
        }

        if (check("FragileBreakout", triggered, prox,
                   "bb%b=" + std::to_string(tech.bb_valid ? tech.bb_percent_b : -1.0) +
                   " vol5=" + std::to_string(tech.volatility_valid ? tech.volatility_5 : -1.0) +
                   " imbalance=" + std::to_string(micro.book_imbalance_valid ? micro.book_imbalance_5 : -1.0))) {
            return WorldState::FragileBreakout;
        }
    }

    // ── CompressionBeforeExpansion ───────────────────────────
    {
        bool narrow_bb = tech.bb_valid &&
                         tech.bb_bandwidth < config_.compression.bb_bandwidth_max;
        bool low_atr = tech.atr_valid &&
                       tech.atr_14_normalized < config_.compression.atr_normalized_max;
        bool low_vol = tech.volatility_valid &&
                       tech.volatility_5 < config_.compression.volatility_5_max;
        bool triggered = narrow_bb && low_atr && low_vol;

        double prox = 0.0;
        if (tech.bb_valid && tech.atr_valid && tech.volatility_valid) {
            double p1 = 1.0 - (tech.bb_bandwidth / config_.compression.bb_bandwidth_max);
            double p2 = 1.0 - (tech.atr_14_normalized / config_.compression.atr_normalized_max);
            double p3 = 1.0 - (tech.volatility_5 / config_.compression.volatility_5_max);
            prox = std::clamp(std::min({p1, p2, p3}), 0.0, 1.0);
        }

        if (check("CompressionBeforeExpansion", triggered, prox,
                   "bb_bw=" + std::to_string(tech.bb_valid ? tech.bb_bandwidth : -1.0) +
                   " atr_norm=" + std::to_string(tech.atr_valid ? tech.atr_14_normalized : -1.0) +
                   " vol5=" + std::to_string(tech.volatility_valid ? tech.volatility_5 : -1.0))) {
            return WorldState::CompressionBeforeExpansion;
        }
    }

    // ── StableTrendContinuation ──────────────────────────────
    {
        bool strong_adx = tech.adx_valid && tech.adx > config_.stable_trend.adx_min;
        bool moderate_rsi = tech.rsi_valid &&
                            tech.rsi_14 >= config_.stable_trend.rsi_lower &&
                            tech.rsi_14 <= config_.stable_trend.rsi_upper;
        bool triggered = strong_adx && moderate_rsi;

        double prox = 0.0;
        if (tech.adx_valid && tech.rsi_valid) {
            double p1 = tech.adx / config_.stable_trend.adx_min;
            double rsi_center = (config_.stable_trend.rsi_lower + config_.stable_trend.rsi_upper) / 2.0;
            double rsi_range = (config_.stable_trend.rsi_upper - config_.stable_trend.rsi_lower) / 2.0;
            double p2 = 1.0 - std::abs(tech.rsi_14 - rsi_center) / rsi_range;
            prox = std::clamp(std::min(p1, std::max(p2, 0.0)), 0.0, 1.0);
        }

        if (check("StableTrendContinuation", triggered, prox,
                   "adx=" + std::to_string(tech.adx_valid ? tech.adx : -1.0) +
                   " rsi=" + std::to_string(tech.rsi_valid ? tech.rsi_14 : -1.0))) {
            return WorldState::StableTrendContinuation;
        }
    }

    // ── ChopNoise ────────────────────────────────────────────
    {
        bool low_adx = tech.adx_valid && tech.adx < config_.chop_noise.adx_max;
        bool mid_rsi = tech.rsi_valid &&
                       tech.rsi_14 >= config_.chop_noise.rsi_lower &&
                       tech.rsi_14 <= config_.chop_noise.rsi_upper;
        bool tight_spread = micro.spread_valid &&
                            micro.spread_bps < config_.chop_noise.spread_bps_max;
        bool triggered = low_adx && mid_rsi && tight_spread;

        double prox = 0.0;
        if (tech.adx_valid && tech.rsi_valid && micro.spread_valid) {
            double p1 = 1.0 - (tech.adx / config_.chop_noise.adx_max);
            double rsi_center = (config_.chop_noise.rsi_lower + config_.chop_noise.rsi_upper) / 2.0;
            double rsi_range = (config_.chop_noise.rsi_upper - config_.chop_noise.rsi_lower) / 2.0;
            double p2 = 1.0 - std::abs(tech.rsi_14 - rsi_center) / rsi_range;
            double p3 = 1.0 - (micro.spread_bps / config_.chop_noise.spread_bps_max);
            prox = std::clamp(std::min({p1, std::max(p2, 0.0), p3}), 0.0, 1.0);
        }

        if (check("ChopNoise", triggered, prox,
                   "adx=" + std::to_string(tech.adx_valid ? tech.adx : -1.0) +
                   " rsi=" + std::to_string(tech.rsi_valid ? tech.rsi_14 : -1.0) +
                   " spread=" + std::to_string(micro.spread_valid ? micro.spread_bps : -1.0))) {
            return WorldState::ChopNoise;
        }
    }

    // Ничего не сработало
    {
        ClassificationCondition cond;
        cond.rule_name = "NoRuleTriggered";
        cond.triggered = true;
        cond.detail = "Ни одно правило не подошло";
        explanation.triggered_conditions.push_back(cond);
        explanation.checked_conditions.push_back(cond);
    }
    return WorldState::Unknown;
}

// ============================================================
// apply_hysteresis() — сглаживание переходов
// ============================================================

WorldState RuleBasedWorldModelEngine::apply_hysteresis(
    WorldState immediate, SymbolContext& ctx,
    WorldModelExplanation& explanation) const {

    if (!config_.hysteresis.enabled) {
        return immediate;
    }

    auto current_confirmed = ctx.confirmed_snapshot.state;

    // Первый вызов — нет предыдущего состояния
    if (current_confirmed == WorldState::Unknown && ctx.dwell_ticks == 0) {
        ctx.candidate_state = immediate;
        ctx.candidate_ticks = 1;
        return immediate;
    }

    // Немедленные переходы для опасных/критических состояний (без гистерезиса)
    if (immediate == WorldState::LiquidityVacuum ||
        immediate == WorldState::ToxicMicrostructure ||
        immediate == WorldState::ExhaustionSpike) {
        ctx.candidate_state = immediate;
        ctx.candidate_ticks = config_.hysteresis.confirmation_ticks;
        return immediate;
    }

    // PostShockStabilization — естественный преемник шоковых состояний
    if (immediate == WorldState::PostShockStabilization &&
        (current_confirmed == WorldState::ExhaustionSpike ||
         current_confirmed == WorldState::LiquidityVacuum)) {
        ctx.candidate_state = immediate;
        ctx.candidate_ticks = config_.hysteresis.confirmation_ticks;
        return immediate;
    }

    if (immediate == current_confirmed) {
        // Остаёмся в том же состоянии — сбрасываем кандидата
        ctx.candidate_state = immediate;
        ctx.candidate_ticks = 0;
        return current_confirmed;
    }

    // Новый кандидат или продолжение текущего кандидата
    if (immediate == ctx.candidate_state) {
        ++ctx.candidate_ticks;
    } else {
        ctx.candidate_state = immediate;
        ctx.candidate_ticks = 1;
    }

    // Проверяем условия перехода
    bool enough_ticks = ctx.candidate_ticks >= config_.hysteresis.confirmation_ticks;
    bool enough_dwell = ctx.dwell_ticks >= config_.hysteresis.min_dwell_ticks;

    if (enough_ticks && enough_dwell) {
        explanation.hysteresis_overrode = false;
        explanation.confirmation_ticks_remaining = 0;
        return immediate;
    }

    // Гистерезис заблокировал переход
    explanation.hysteresis_overrode = true;
    explanation.confirmation_ticks_remaining =
        std::max(0, config_.hysteresis.confirmation_ticks - ctx.candidate_ticks);
    explanation.dwell_ticks = ctx.dwell_ticks;

    return current_confirmed;
}

// ============================================================
// compute_fragility() — composite probabilistic fragility
// ============================================================

FragilityScore RuleBasedWorldModelEngine::compute_fragility(
    const features::FeatureSnapshot& snap,
    WorldState state,
    const SymbolContext& ctx) const {

    FragilityScore score;
    score.valid = true;

    const auto& cfg = config_.fragility;

    // 1. Базовая хрупкость по состоянию
    double base = cfg.unknown;
    switch (state) {
        case WorldState::StableTrendContinuation:    base = cfg.stable_trend; break;
        case WorldState::FragileBreakout:            base = cfg.fragile_breakout; break;
        case WorldState::CompressionBeforeExpansion: base = cfg.compression; break;
        case WorldState::ChopNoise:                  base = cfg.chop_noise; break;
        case WorldState::ExhaustionSpike:            base = cfg.exhaustion_spike; break;
        case WorldState::LiquidityVacuum:            base = cfg.liquidity_vacuum; break;
        case WorldState::ToxicMicrostructure:        base = cfg.toxic_micro; break;
        case WorldState::PostShockStabilization:     base = cfg.post_shock; break;
        case WorldState::Unknown:                    score.valid = false; break;
    }

    // 2. Spread stress
    double spread_component = 0.0;
    if (snap.microstructure.spread_valid && cfg.spread_normalization > 0.0) {
        spread_component = std::min(1.0, snap.microstructure.spread_bps / cfg.spread_normalization);
    }

    // 3. Book instability
    double instability_component = 0.0;
    if (snap.microstructure.instability_valid) {
        instability_component = snap.microstructure.book_instability;
    }

    // 4. Volatility acceleration: |vol_5 - vol_20| / max(vol_20, 0.001)
    double vol_accel_component = 0.0;
    if (snap.technical.volatility_valid && snap.technical.volatility_20 > 0.001) {
        double accel = std::abs(snap.technical.volatility_5 - snap.technical.volatility_20)
                       / snap.technical.volatility_20;
        vol_accel_component = std::min(1.0, accel);
    }

    // 5. Liquidity imbalance
    double liq_component = 0.0;
    if (snap.microstructure.liquidity_valid) {
        liq_component = 1.0 - snap.microstructure.liquidity_ratio;
    }

    // 6. Transition instability (из истории)
    double trans_component = 0.0;
    if (!ctx.history.empty()) {
        int recent_transitions = ctx.recent_transition_count(config_.history.tendency_lookback * 2);
        trans_component = std::min(1.0, static_cast<double>(recent_transitions) / 5.0);
    }

    // 7. VPIN toxicity
    double vpin_component = 0.0;
    if (snap.microstructure.vpin_valid) {
        vpin_component = snap.microstructure.vpin;
    }

    // Composite: взвешенная сумма компонент поверх base
    double adjustment = spread_component * cfg.spread_stress_weight
                      + instability_component * cfg.book_instability_weight
                      + vol_accel_component * cfg.volatility_accel_weight
                      + liq_component * cfg.liquidity_imbalance_weight
                      + trans_component * cfg.transition_instability_weight
                      + vpin_component * cfg.vpin_toxicity_weight;

    score.value = std::clamp(base + adjustment, cfg.floor, cfg.ceiling);

    // Confidence в оценке fragility
    int components_available = 0;
    if (snap.microstructure.spread_valid)     ++components_available;
    if (snap.microstructure.instability_valid) ++components_available;
    if (snap.technical.volatility_valid)       ++components_available;
    if (snap.microstructure.liquidity_valid)    ++components_available;
    if (snap.microstructure.vpin_valid)         ++components_available;
    score.confidence = static_cast<double>(components_available) / 5.0;

    return score;
}

// ============================================================
// compute_persistence() — blend base + empirical
// ============================================================

double RuleBasedWorldModelEngine::compute_persistence(WorldState state, const SymbolContext& ctx) const {
    const auto& cfg = config_.persistence;

    double base = cfg.unknown;
    switch (state) {
        case WorldState::StableTrendContinuation:    base = cfg.stable_trend; break;
        case WorldState::FragileBreakout:            base = cfg.fragile_breakout; break;
        case WorldState::CompressionBeforeExpansion: base = cfg.compression; break;
        case WorldState::ChopNoise:                  base = cfg.chop_noise; break;
        case WorldState::ExhaustionSpike:            base = cfg.exhaustion_spike; break;
        case WorldState::LiquidityVacuum:            base = cfg.liquidity_vacuum; break;
        case WorldState::ToxicMicrostructure:        base = cfg.toxic_micro; break;
        case WorldState::PostShockStabilization:     base = cfg.post_shock; break;
        case WorldState::Unknown:                    break;
    }

    // Blend с эмпирическими данными
    int total = ctx.state_total_count[state_index(state)];
    if (total >= static_cast<int>(cfg.min_history_for_empirical)) {
        double empirical = ctx.empirical_persistence(state);
        return base * (1.0 - cfg.history_blend_weight) + empirical * cfg.history_blend_weight;
    }

    return base;
}

// ============================================================
// compute_transition() — velocity, pressure, tendency
// ============================================================

TransitionContext RuleBasedWorldModelEngine::compute_transition(
    WorldState current, const SymbolContext& ctx) const {

    TransitionContext t;
    t.previous_state = ctx.confirmed_snapshot.state;

    // Tendency (улучшенная версия)
    if (current == t.previous_state) {
        t.tendency = TransitionTendency::Stable;
    } else {
        int current_q = state_quality(current);
        int previous_q = state_quality(t.previous_state);
        if (current_q > previous_q) {
            t.tendency = TransitionTendency::Improving;
        } else if (current_q < previous_q) {
            t.tendency = TransitionTendency::Deteriorating;
        } else {
            t.tendency = TransitionTendency::Ambiguous;
        }
    }

    // Velocity: скользящая разность quality за последние N тиков
    size_t lookback = config_.history.tendency_lookback;
    if (ctx.history.size() >= 2) {
        size_t n = std::min(lookback, ctx.history.size());
        auto newest = ctx.history.rbegin();
        auto oldest = newest;
        std::advance(oldest, std::min(n - 1, ctx.history.size() - 1));
        int q_new = state_quality(newest->state);
        int q_old = state_quality(oldest->state);
        t.velocity = std::clamp(static_cast<double>(q_new - q_old) / static_cast<double>(n), -1.0, 1.0);
    }

    // Pressure: вероятность ухода из текущего состояния
    // = 1.0 - empirical_persistence (чем ниже персистентность, тем выше давление)
    double emp = ctx.empirical_persistence(current);
    t.pressure = std::clamp(1.0 - emp, 0.0, 1.0);

    // Недавние переходы
    t.transitions_recent = ctx.recent_transition_count(lookback);

    return t;
}

// ============================================================
// compute_confidence() — общая уверенность в классификации
// ============================================================

double RuleBasedWorldModelEngine::compute_confidence(
    const features::FeatureSnapshot& snap,
    WorldState state,
    const SymbolContext& ctx,
    const WorldModelExplanation& explanation) const {

    // Компоненты confidence:
    // 1. Data quality (30%)
    double dq = assess_data_quality(snap);

    // 2. Indicator coverage (20%)
    double coverage = explanation.total_indicator_count > 0
                      ? static_cast<double>(explanation.valid_indicator_count) / explanation.total_indicator_count
                      : 0.0;

    // 3. State stability — dwell time (20%)
    double stability = std::min(1.0, static_cast<double>(ctx.dwell_ticks) / 10.0);

    // 4. Hysteresis agreement (15%): если гистерезис не менял результат → выше confidence
    double hysteresis_factor = explanation.hysteresis_overrode ? 0.5 : 1.0;

    // 5. Rule proximity (15%): самое сильное срабатывание
    double max_proximity = 0.0;
    for (const auto& cond : explanation.triggered_conditions) {
        max_proximity = std::max(max_proximity, cond.proximity);
    }

    double confidence = dq * 0.30
                      + coverage * 0.20
                      + stability * 0.20
                      + hysteresis_factor * 0.15
                      + max_proximity * 0.15;

    // Штраф для Unknown
    if (state == WorldState::Unknown) {
        confidence *= 0.3;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

// ============================================================
// compute_state_probabilities()
// ============================================================

StateProbabilities RuleBasedWorldModelEngine::compute_state_probabilities(
    WorldState primary, double confidence, const SymbolContext& ctx) const {

    StateProbabilities probs;
    probs.valid = true;

    int pidx = state_index(primary);

    // Начинаем с uniform prior
    double remaining = 1.0 - confidence;
    for (size_t i = 0; i < kWorldStateCount; ++i) {
        probs.values[i] = remaining / static_cast<double>(kWorldStateCount);
    }
    probs.values[pidx] += confidence;

    // Корректируем на основе матрицы переходов (если есть данные)
    if (ctx.total_transitions > 20) {
        auto prev = ctx.confirmed_snapshot.state;
        int prev_idx = state_index(prev);

        // Вычисляем row sum
        int row_sum = 0;
        for (size_t j = 0; j < kWorldStateCount; ++j) {
            row_sum += ctx.transition_counts[prev_idx][j];
        }

        if (row_sum > 0) {
            double blend = 0.2; // 20% transition prior
            for (size_t i = 0; i < kWorldStateCount; ++i) {
                double tp = static_cast<double>(ctx.transition_counts[prev_idx][i]) / row_sum;
                probs.values[i] = probs.values[i] * (1.0 - blend) + tp * blend;
            }
        }
    }

    // Нормализация
    double sum = 0.0;
    for (size_t i = 0; i < kWorldStateCount; ++i) sum += probs.values[i];
    if (sum > 0.0) {
        for (size_t i = 0; i < kWorldStateCount; ++i) probs.values[i] /= sum;
    }

    return probs;
}

// ============================================================
// compute_suitability() — multi-dimension с feedback
// ============================================================

std::vector<StrategySuitability> RuleBasedWorldModelEngine::compute_suitability(
    WorldState state,
    const features::FeatureSnapshot& snap,
    const SymbolContext& ctx) const {

    std::vector<StrategySuitability> result;
    int si = state_index(state);

    auto it = config_.suitability.table.find(si);
    if (it == config_.suitability.table.end()) {
        // Fallback: Unknown entry
        it = config_.suitability.table.find(state_index(WorldState::Unknown));
        if (it == config_.suitability.table.end()) return result;
    }

    for (const auto& entry : it->second) {
        StrategySuitability suit;
        suit.strategy_id = StrategyId(entry.strategy_id);
        suit.reason = entry.reason;

        // Signal suitability (из таблицы конфигурации)
        suit.signal_suitability = entry.suitability;

        // Execution suitability: штраф при плохом исполнении
        suit.execution_suitability = 1.0;
        if (snap.microstructure.spread_valid && snap.microstructure.spread_bps > 30.0) {
            suit.execution_suitability = std::max(0.2, 1.0 - (snap.microstructure.spread_bps - 30.0) / 100.0);
        }
        if (snap.execution_context.slippage_valid && snap.execution_context.estimated_slippage_bps > 10.0) {
            suit.execution_suitability *= std::max(0.3, 1.0 - snap.execution_context.estimated_slippage_bps / 50.0);
        }

        // Risk suitability: штраф при опасных состояниях
        suit.risk_suitability = 1.0;
        if (state == WorldState::LiquidityVacuum || state == WorldState::ToxicMicrostructure) {
            suit.risk_suitability = 0.1;
        } else if (state == WorldState::ExhaustionSpike) {
            suit.risk_suitability = 0.3;
        }

        // Feedback adjustment: blend с исторической производительностью
        std::string feedback_key = std::to_string(si) + ":" + entry.strategy_id;
        auto fb_it = feedback_stats_.find(feedback_key);
        if (fb_it != feedback_stats_.end() &&
            fb_it->second.total_trades >= static_cast<int>(config_.suitability.min_trades_for_feedback)) {

            double historical_score = fb_it->second.ema_win_rate;
            double blend = config_.suitability.feedback_blend_weight;
            suit.signal_suitability = suit.signal_suitability * (1.0 - blend) + historical_score * blend;
        }

        // Итоговый suitability = min(signal × execution × risk) для безопасности
        suit.suitability = suit.signal_suitability * suit.execution_suitability * suit.risk_suitability;

        // Veto
        if (suit.suitability < config_.suitability.hard_veto_threshold) {
            suit.vetoed = true;
            suit.suitability = 0.0;
            suit.reason += " | VETO: suitability ниже порога";
        }

        result.push_back(std::move(suit));
    }

    return result;
}

// ============================================================
// compute_top_drivers()
// ============================================================

std::vector<std::pair<std::string, double>> RuleBasedWorldModelEngine::compute_top_drivers(
    const features::FeatureSnapshot& snap, WorldState state) const {

    std::vector<std::pair<std::string, double>> drivers;

    if (snap.technical.adx_valid)
        drivers.emplace_back("ADX", snap.technical.adx);
    if (snap.technical.rsi_valid)
        drivers.emplace_back("RSI", snap.technical.rsi_14);
    if (snap.technical.bb_valid) {
        drivers.emplace_back("BB%B", snap.technical.bb_percent_b);
        drivers.emplace_back("BB_BW", snap.technical.bb_bandwidth);
    }
    if (snap.technical.volatility_valid) {
        drivers.emplace_back("Vol5", snap.technical.volatility_5);
        drivers.emplace_back("Vol20", snap.technical.volatility_20);
    }
    if (snap.technical.momentum_valid)
        drivers.emplace_back("Momentum", snap.technical.momentum_5);
    if (snap.microstructure.spread_valid)
        drivers.emplace_back("Spread_bps", snap.microstructure.spread_bps);
    if (snap.microstructure.instability_valid)
        drivers.emplace_back("BookInstability", snap.microstructure.book_instability);
    if (snap.microstructure.trade_flow_valid)
        drivers.emplace_back("AggressiveFlow", snap.microstructure.aggressive_flow);
    if (snap.microstructure.liquidity_valid)
        drivers.emplace_back("LiquidityRatio", snap.microstructure.liquidity_ratio);
    if (snap.microstructure.vpin_valid)
        drivers.emplace_back("VPIN", snap.microstructure.vpin);

    // Сортируем по абсолютной значимости (descending)
    std::sort(drivers.begin(), drivers.end(),
              [](const auto& a, const auto& b) { return std::abs(a.second) > std::abs(b.second); });

    // Top-5
    if (drivers.size() > 5) drivers.resize(5);
    return drivers;
}

// ============================================================
// generate_summary()
// ============================================================

std::string RuleBasedWorldModelEngine::generate_summary(
    const WorldModelExplanation& explanation,
    const FragilityScore& fragility,
    double confidence) const {

    std::ostringstream ss;
    ss << to_string(explanation.confirmed_state);
    ss << " (conf=" << std::to_string(confidence).substr(0, 4);
    ss << " frag=" << std::to_string(fragility.value).substr(0, 4);
    ss << " dq=" << std::to_string(explanation.data_quality_score).substr(0, 4);
    ss << " ind=" << explanation.valid_indicator_count << "/" << explanation.total_indicator_count;

    if (explanation.hysteresis_overrode) {
        ss << " HYST:blocked→" << to_string(explanation.immediate_state);
    }

    if (!explanation.triggered_conditions.empty()) {
        ss << " rules=[";
        for (size_t i = 0; i < explanation.triggered_conditions.size() && i < 3; ++i) {
            if (i > 0) ss << ",";
            ss << explanation.triggered_conditions[i].rule_name;
        }
        ss << "]";
    }

    ss << ")";
    return ss.str();
}

// ============================================================
// assess_data_quality()
// ============================================================

double RuleBasedWorldModelEngine::assess_data_quality(const features::FeatureSnapshot& snap) const {
    int available = 0;
    int total = 8;

    if (snap.technical.sma_valid)        ++available;
    if (snap.technical.rsi_valid)        ++available;
    if (snap.technical.adx_valid)        ++available;
    if (snap.technical.bb_valid)         ++available;
    if (snap.technical.volatility_valid) ++available;
    if (snap.microstructure.spread_valid) ++available;
    if (snap.microstructure.instability_valid) ++available;
    if (snap.microstructure.vpin_valid)  ++available;

    double base_quality = static_cast<double>(available) / total;

    // Штраф за устаревшие данные
    if (!snap.execution_context.is_feed_fresh) {
        base_quality *= 0.7;
    }

    return std::clamp(base_quality, 0.0, 1.0);
}

// ============================================================
// state_quality() — ранг для tendency
// ============================================================

int RuleBasedWorldModelEngine::state_quality(WorldState s) {
    switch (s) {
        case WorldState::StableTrendContinuation:    return 3;
        case WorldState::ChopNoise:                  return 2;
        case WorldState::PostShockStabilization:      return 2;
        case WorldState::CompressionBeforeExpansion:  return 1;
        case WorldState::FragileBreakout:             return 0;
        case WorldState::ExhaustionSpike:             return -1;
        case WorldState::ToxicMicrostructure:         return -2;
        case WorldState::LiquidityVacuum:             return -3;
        case WorldState::Unknown:                     return 0;
    }
    return 0;
}

// ============================================================
// record_feedback() — запись обратной связи
// ============================================================

void RuleBasedWorldModelEngine::record_feedback(const WorldStateFeedback& feedback) {
    std::lock_guard lock(mutex_);

    std::string key = std::to_string(state_index(feedback.state)) + ":" + feedback.strategy_id.get();
    auto& stats = feedback_stats_[key];

    ++stats.total_trades;
    if (feedback.was_profitable) ++stats.winning_trades;
    stats.total_pnl_bps += feedback.pnl_bps;

    // EMA updates
    double alpha = config_.feedback.ema_alpha;
    stats.ema_win_rate = stats.ema_win_rate * (1.0 - alpha) + (feedback.was_profitable ? 1.0 : 0.0) * alpha;
    stats.ema_expectancy = stats.ema_expectancy * (1.0 - alpha) + feedback.pnl_bps * alpha;

    // Slippage
    stats.avg_slippage_bps = stats.avg_slippage_bps * (1.0 - alpha) + feedback.slippage_bps * alpha;

    // Max drawdown
    stats.max_drawdown_bps = std::max(stats.max_drawdown_bps, feedback.max_adverse_excursion_bps);

    logger_->debug("WorldModel",
        "Feedback записан: state=" + to_string(feedback.state) +
        " strategy=" + feedback.strategy_id.get() +
        " pnl=" + std::to_string(feedback.pnl_bps) + "bps",
        {{"key", key},
         {"total_trades", std::to_string(stats.total_trades)},
         {"ema_wr", std::to_string(stats.ema_win_rate)}});
}

// ============================================================
// performance_stats()
// ============================================================

std::optional<StatePerformanceStats> RuleBasedWorldModelEngine::performance_stats(
    WorldState state, const StrategyId& strategy) const {

    std::lock_guard lock(mutex_);
    std::string key = std::to_string(state_index(state)) + ":" + strategy.get();
    auto it = feedback_stats_.find(key);
    if (it != feedback_stats_.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace tb::world_model
