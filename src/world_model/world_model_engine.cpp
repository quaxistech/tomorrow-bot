#include "world_model/world_model_engine.hpp"
#include <algorithm>
#include <cmath>

namespace tb::world_model {

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
            return WorldStateLabel::Transitioning;
        case WorldState::ExhaustionSpike:
        case WorldState::LiquidityVacuum:
        case WorldState::ToxicMicrostructure:
            return WorldStateLabel::Disrupted;
        case WorldState::ChopNoise:
            return WorldStateLabel::Transitioning;
        case WorldState::Unknown:
            return WorldStateLabel::Unknown;
    }
    return WorldStateLabel::Unknown;
}

// ============================================================
// RuleBasedWorldModelEngine
// ============================================================

RuleBasedWorldModelEngine::RuleBasedWorldModelEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
{}

WorldModelSnapshot RuleBasedWorldModelEngine::update(const features::FeatureSnapshot& snapshot) {
    const auto& sym = snapshot.symbol.get();

    // Классификация состояния
    auto state = classify_state(snapshot);
    auto fragility = compute_fragility(snapshot, state);

    // Определяем предыдущее состояние для вычисления тенденции
    WorldState previous_state = WorldState::Unknown;
    {
        std::lock_guard lock(mutex_);
        auto it = states_.find(sym);
        if (it != states_.end()) {
            previous_state = it->second.state;
        }
    }

    auto tendency = compute_tendency(state, previous_state);
    auto suitability = compute_suitability(state, snapshot);

    // Вычисляем persistence_score: выше для стабильных состояний
    double persistence = 0.5;
    if (state == WorldState::StableTrendContinuation || state == WorldState::ChopNoise) {
        persistence = 0.75;
    } else if (state == WorldState::ExhaustionSpike || state == WorldState::LiquidityVacuum) {
        persistence = 0.2; // Нестабильные состояния — низкая персистентность
    } else if (state == WorldState::FragileBreakout) {
        persistence = 0.3;
    }

    WorldModelSnapshot result;
    result.state = state;
    result.label = WorldModelSnapshot::to_label(state);
    result.fragility = fragility;
    result.tendency = tendency;
    result.persistence_score = persistence;
    result.strategy_suitability = std::move(suitability);
    result.computed_at = clock_->now();
    result.symbol = snapshot.symbol;

    // Сохраняем состояние
    {
        std::lock_guard lock(mutex_);
        states_[sym] = result;
    }

    logger_->debug("WorldModel",
                   "Классификация мира: " + to_string(state) + " для " + sym,
                   {{"state", to_string(state)},
                    {"fragility", std::to_string(fragility.value)},
                    {"tendency", to_string(tendency)}});

    return result;
}

std::optional<WorldModelSnapshot> RuleBasedWorldModelEngine::current_state(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(symbol.get());
    if (it != states_.end()) {
        return it->second;
    }
    return std::nullopt;
}

WorldState RuleBasedWorldModelEngine::classify_state(const features::FeatureSnapshot& snap) const {
    const auto& tech = snap.technical;
    const auto& micro = snap.microstructure;

    // Проверяем наличие данных
    if (!tech.sma_valid && !tech.rsi_valid && !micro.spread_valid) {
        return WorldState::Unknown;
    }

    // --- Проверяем предыдущее состояние для PostShockStabilization ---
    {
        std::lock_guard lock(mutex_);
        auto it = states_.find(snap.symbol.get());
        if (it != states_.end()) {
            const auto prev = it->second.state;
            if ((prev == WorldState::ExhaustionSpike || prev == WorldState::LiquidityVacuum) &&
                tech.volatility_valid && tech.volatility_5 < tech.volatility_20) {
                return WorldState::PostShockStabilization;
            }
        }
    }

    // --- ToxicMicrostructure: book_instability > 0.7, aggressive_flow > 0.8 ---
    if (micro.instability_valid && micro.book_instability > 0.7 &&
        micro.trade_flow_valid && micro.aggressive_flow > 0.8 &&
        micro.spread_valid && micro.spread_bps > 15.0) {
        return WorldState::ToxicMicrostructure;
    }

    // --- LiquidityVacuum: очень широкий спред или перекошенная ликвидность ---
    if (micro.spread_valid && micro.spread_bps > 50.0) {
        return WorldState::LiquidityVacuum;
    }
    // liquidity_ratio = min(bid,ask) / ((bid+ask)/2), диапазон [0, 1].
    // Значение < 0.3 означает сильный перекос глубины стакана.
    if (micro.liquidity_valid && micro.liquidity_ratio < 0.3 &&
        micro.spread_valid && micro.spread_bps > 20.0) {
        return WorldState::LiquidityVacuum;
    }

    // --- ExhaustionSpike: экстремальный RSI + высокий momentum ---
    if (tech.rsi_valid && (tech.rsi_14 > 80.0 || tech.rsi_14 < 20.0) &&
        tech.momentum_valid && std::abs(tech.momentum_5) > 0.02) {
        return WorldState::ExhaustionSpike;
    }

    // --- FragileBreakout: percent_b на краях + высокая волатильность + дисбаланс стакана ---
    if (tech.bb_valid && (tech.bb_percent_b > 0.95 || tech.bb_percent_b < 0.05) &&
        tech.volatility_valid && tech.volatility_5 > 0.02 &&
        micro.book_imbalance_valid && std::abs(micro.book_imbalance_5) > 0.3) {
        return WorldState::FragileBreakout;
    }

    // --- CompressionBeforeExpansion: узкий BB bandwidth + снижающийся ATR ---
    if (tech.bb_valid && tech.bb_bandwidth < 0.03 &&
        tech.atr_valid && tech.atr_14_normalized < 0.01 &&
        tech.volatility_valid && tech.volatility_5 < 0.01) {
        return WorldState::CompressionBeforeExpansion;
    }

    // --- StableTrendContinuation: ADX > 25, RSI 40-70, низкая хрупкость ---
    if (tech.adx_valid && tech.adx > 25.0 &&
        tech.rsi_valid && tech.rsi_14 >= 40.0 && tech.rsi_14 <= 70.0) {
        return WorldState::StableTrendContinuation;
    }

    // --- ChopNoise: ADX < 20, RSI 40-60, узкий спред ---
    if (tech.adx_valid && tech.adx < 20.0 &&
        tech.rsi_valid && tech.rsi_14 >= 40.0 && tech.rsi_14 <= 60.0 &&
        micro.spread_valid && micro.spread_bps < 20.0) {
        return WorldState::ChopNoise;
    }

    return WorldState::Unknown;
}

FragilityScore RuleBasedWorldModelEngine::compute_fragility(
    const features::FeatureSnapshot& snap, WorldState state) const {

    FragilityScore score;
    score.valid = true;

    // Базовая хрупкость зависит от состояния
    switch (state) {
        case WorldState::StableTrendContinuation:
            score.value = 0.2;
            break;
        case WorldState::FragileBreakout:
            score.value = 0.8;
            break;
        case WorldState::CompressionBeforeExpansion:
            score.value = 0.6;
            break;
        case WorldState::ChopNoise:
            score.value = 0.3;
            break;
        case WorldState::ExhaustionSpike:
            score.value = 0.9;
            break;
        case WorldState::LiquidityVacuum:
            score.value = 0.95;
            break;
        case WorldState::ToxicMicrostructure:
            score.value = 0.85;
            break;
        case WorldState::PostShockStabilization:
            score.value = 0.5;
            break;
        case WorldState::Unknown:
            score.value = 0.5;
            score.valid = false;
            break;
    }

    // Корректируем на основе микроструктуры
    if (snap.microstructure.spread_valid) {
        // Широкий спред увеличивает хрупкость
        double spread_factor = std::min(1.0, snap.microstructure.spread_bps / 100.0);
        score.value = std::min(1.0, score.value + spread_factor * 0.1);
    }

    if (snap.microstructure.instability_valid) {
        // Нестабильность стакана увеличивает хрупкость
        score.value = std::min(1.0, score.value + snap.microstructure.book_instability * 0.15);
    }

    return score;
}

TransitionTendency RuleBasedWorldModelEngine::compute_tendency(
    WorldState current, WorldState previous) const {

    if (current == previous) {
        return TransitionTendency::Stable;
    }

    // Определяем «позитивные» и «негативные» состояния
    // Позитивные: StableTrendContinuation, ChopNoise (стабильный), PostShockStabilization
    // Негативные: LiquidityVacuum, ToxicMicrostructure, ExhaustionSpike

    auto state_quality = [](WorldState s) -> int {
        switch (s) {
            case WorldState::StableTrendContinuation: return 3;
            case WorldState::ChopNoise:               return 2;
            case WorldState::PostShockStabilization:   return 2;
            case WorldState::CompressionBeforeExpansion: return 1;
            case WorldState::FragileBreakout:          return 0;
            case WorldState::ExhaustionSpike:          return -1;
            case WorldState::ToxicMicrostructure:      return -2;
            case WorldState::LiquidityVacuum:          return -3;
            case WorldState::Unknown:                  return 0;
        }
        return 0;
    };

    int current_q = state_quality(current);
    int previous_q = state_quality(previous);

    if (current_q > previous_q) {
        return TransitionTendency::Improving;
    } else if (current_q < previous_q) {
        return TransitionTendency::Deteriorating;
    }
    return TransitionTendency::Ambiguous;
}

std::vector<StrategySuitability> RuleBasedWorldModelEngine::compute_suitability(
    WorldState state, const features::FeatureSnapshot& /*snap*/) const {

    std::vector<StrategySuitability> result;

    // Маппинг состояний мира на пригодность для стратегий
    auto add = [&](const std::string& id, double suit, const std::string& reason) {
        result.push_back({StrategyId(id), suit, reason});
    };

    switch (state) {
        case WorldState::StableTrendContinuation:
            add("momentum",            0.9, "Устойчивый тренд — идеально для momentum");
            add("mean_reversion",      0.2, "Тренд — не подходит для возврата к среднему");
            add("breakout",            0.4, "Тренд уже установлен, пробой менее вероятен");
            add("microstructure_scalp",0.5, "Стабильная микроструктура допускает скальпинг");
            add("vol_expansion",       0.3, "Волатильность стабильна, расширение маловероятно");
            break;
        case WorldState::FragileBreakout:
            add("momentum",            0.5, "Пробой может перерасти в тренд");
            add("mean_reversion",      0.1, "Пробой — опасно торговать против");
            add("breakout",            0.8, "Пробой — основной сценарий");
            add("microstructure_scalp",0.3, "Хрупкая микроструктура");
            add("vol_expansion",       0.7, "Волатильность расширяется");
            break;
        case WorldState::CompressionBeforeExpansion:
            add("momentum",            0.2, "Ещё нет направления");
            add("mean_reversion",      0.6, "Можно торговать в диапазоне до пробоя");
            add("breakout",            0.9, "Сжатие — лучший сетап для пробоя");
            add("microstructure_scalp",0.6, "Узкие спреды в сжатии");
            add("vol_expansion",       0.8, "Ожидание расширения волатильности");
            break;
        case WorldState::ChopNoise:
            add("momentum",            0.1, "Нет тренда — momentum неэффективен");
            add("mean_reversion",      0.7, "Боковик — хорошо для mean reversion");
            add("breakout",            0.1, "Ложные пробои в боковике");
            add("microstructure_scalp",0.8, "Стабильный стакан — хорошо для скальпинга");
            add("vol_expansion",       0.1, "Низкая волатильность, расширение маловероятно");
            break;
        case WorldState::ExhaustionSpike:
            add("momentum",            0.1, "Импульс истощён");
            add("mean_reversion",      0.6, "Возврат после спайка вероятен");
            add("breakout",            0.1, "Ложный сигнал пробоя");
            add("microstructure_scalp",0.1, "Нестабильная микроструктура");
            add("vol_expansion",       0.3, "Волатильность уже высока");
            break;
        case WorldState::LiquidityVacuum:
            add("momentum",            0.05, "Вакуум ликвидности — торговля опасна");
            add("mean_reversion",      0.05, "Вакуум ликвидности — торговля опасна");
            add("breakout",            0.05, "Вакуум ликвидности — торговля опасна");
            add("microstructure_scalp",0.0,  "Вакуум ликвидности — скальпинг невозможен");
            add("vol_expansion",       0.05, "Вакуум ликвидности — торговля опасна");
            break;
        case WorldState::ToxicMicrostructure:
            add("momentum",            0.1, "Токсичный поток искажает сигналы");
            add("mean_reversion",      0.1, "Токсичная микроструктура");
            add("breakout",            0.1, "Манипулятивные пробои");
            add("microstructure_scalp",0.0, "Микроструктура токсична — скальпинг запрещён");
            add("vol_expansion",       0.2, "Возможна повышенная волатильность");
            break;
        case WorldState::PostShockStabilization:
            add("momentum",            0.3, "Направление после шока неясно");
            add("mean_reversion",      0.5, "Возврат к нормальным уровням");
            add("breakout",            0.2, "Повторный шок маловероятен");
            add("microstructure_scalp",0.4, "Микроструктура стабилизируется");
            add("vol_expansion",       0.4, "Волатильность снижается");
            break;
        case WorldState::Unknown:
            add("momentum",            0.3, "Состояние неизвестно — пониженный вес");
            add("mean_reversion",      0.3, "Состояние неизвестно — пониженный вес");
            add("breakout",            0.3, "Состояние неизвестно — пониженный вес");
            add("microstructure_scalp",0.3, "Состояние неизвестно — пониженный вес");
            add("vol_expansion",       0.3, "Состояние неизвестно — пониженный вес");
            break;
    }

    return result;
}

} // namespace tb::world_model
