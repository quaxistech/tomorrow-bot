#include "uncertainty/uncertainty_engine.hpp"
#include "portfolio/portfolio_types.hpp"
#include "ml/ml_signal_types.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::uncertainty {

// ============================================================
// Преобразования
// ============================================================

std::string to_string(UncertaintyAction action) {
    switch (action) {
        case UncertaintyAction::Normal:          return "Normal";
        case UncertaintyAction::ReducedSize:     return "ReducedSize";
        case UncertaintyAction::HigherThreshold: return "HigherThreshold";
        case UncertaintyAction::NoTrade:         return "NoTrade";
    }
    return "Normal";
}

std::string to_string(ExecutionModeRecommendation mode) {
    switch (mode) {
        case ExecutionModeRecommendation::Normal:         return "Normal";
        case ExecutionModeRecommendation::Conservative:   return "Conservative";
        case ExecutionModeRecommendation::DefensiveOnly:  return "DefensiveOnly";
        case ExecutionModeRecommendation::HaltNewEntries: return "HaltNewEntries";
    }
    return "Normal";
}

// ============================================================
// Constructor
// ============================================================

RuleBasedUncertaintyEngine::RuleBasedUncertaintyEngine(
    UncertaintyConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , diagnostics_{}
{}

// ============================================================
// v1 assess() — делегирует в v2 с нейтральными снэпшотами
// ============================================================

UncertaintySnapshot RuleBasedUncertaintyEngine::assess(
    const features::FeatureSnapshot& features,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world) {

    portfolio::PortfolioSnapshot neutral_portfolio;

    ml::MlSignalSnapshot neutral_ml;
    neutral_ml.overall_health = ml::MlComponentHealth::Healthy;

    return assess(features, regime, world, neutral_portfolio, neutral_ml);
}

// ============================================================
// v2 assess() — полная оценка (CORE)
// ============================================================

UncertaintySnapshot RuleBasedUncertaintyEngine::assess(
    const features::FeatureSnapshot& features,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world,
    const portfolio::PortfolioSnapshot& portfolio,
    const ml::MlSignalSnapshot& ml_signals) {

    const auto now = clock_->now();
    const auto now_ns = now.get();

    // 1. Compute all 9 dimensions
    UncertaintyDimensions dims;
    dims.regime_uncertainty        = compute_regime_uncertainty(regime);
    dims.signal_uncertainty        = compute_signal_uncertainty(features);
    dims.data_quality_uncertainty  = compute_data_quality_uncertainty(features);
    dims.execution_uncertainty     = compute_execution_uncertainty(features);
    dims.portfolio_uncertainty     = compute_portfolio_uncertainty(portfolio);
    dims.ml_uncertainty            = compute_ml_uncertainty(ml_signals);
    dims.correlation_uncertainty   = compute_correlation_uncertainty(ml_signals);
    dims.transition_uncertainty    = compute_transition_uncertainty(regime);
    dims.operational_uncertainty   = compute_operational_uncertainty(features);

    // 2. Aggregate with regime-specific weight adjustment
    double agg = aggregate(dims, regime);

    // 3. World fragility stress amplifier (same as v1)
    if (world.fragility.valid && world.fragility.value > 0.7) {
        agg = std::min(1.0, agg + 0.1);
    }

    // 4. Retrieve/create SymbolState
    SymbolState* state_ptr = nullptr;
    {
        std::lock_guard lock(mutex_);
        state_ptr = &states_[features.symbol.get()];
    }
    auto& state = *state_ptr;

    // 5. EMA smoothing → persistent_score
    double persistent = config_.ema_alpha * agg +
                        (1.0 - config_.ema_alpha) * state.ema_score;

    // 6. spike_score
    double spike = std::max(0.0, agg - persistent);

    // 7. Apply hysteresis to determine final level
    auto level = apply_hysteresis(agg, state);

    // 8. recommended_action from level
    UncertaintyAction action;
    switch (level) {
        case UncertaintyLevel::Low:      action = UncertaintyAction::Normal; break;
        case UncertaintyLevel::Moderate:  action = UncertaintyAction::ReducedSize; break;
        case UncertaintyLevel::High:      action = UncertaintyAction::HigherThreshold; break;
        case UncertaintyLevel::Extreme:   action = UncertaintyAction::NoTrade; break;
    }

    // 9. size_multiplier
    double size_mult = std::max(config_.size_floor, 1.0 - agg);

    // 10. threshold_multiplier
    double threshold_mult = std::min(config_.threshold_ceiling, 1.0 + agg);

    // 11. execution_mode
    auto exec_mode = determine_execution_mode(level, agg, state);

    // 12. cooldown
    auto cooldown = compute_cooldown(state, now_ns);

    // 13. top_drivers (top-3)
    auto drivers = compute_drivers(dims);

    // 14. freshness_ns = 0 (just computed)
    constexpr int64_t freshness = 0;

    // 15. calibration_confidence from diagnostics
    double cal_conf;
    {
        std::lock_guard lock(mutex_);
        if (diagnostics_.feedback_samples < config_.min_feedback_samples) {
            cal_conf = 0.5;
        } else {
            double ratio = static_cast<double>(diagnostics_.feedback_samples) /
                           (2.0 * static_cast<double>(config_.min_feedback_samples));
            cal_conf = std::min(0.95, 0.5 + 0.5 * ratio);
        }
    }

    // Build explanation
    std::ostringstream explanation;
    explanation << "Неопределённость: " << agg << " ("
                << "режим=" << dims.regime_uncertainty
                << ", сигнал=" << dims.signal_uncertainty
                << ", данные=" << dims.data_quality_uncertainty
                << ", исполнение=" << dims.execution_uncertainty
                << ", портфель=" << dims.portfolio_uncertainty
                << ", ML=" << dims.ml_uncertainty
                << ", корреляция=" << dims.correlation_uncertainty
                << ", переход=" << dims.transition_uncertainty
                << ", операционная=" << dims.operational_uncertainty << ")";

    if (world.fragility.valid && world.fragility.value > 0.7) {
        explanation << " | Высокая хрупкость мира: " << world.fragility.value;
    }

    // Build snapshot
    UncertaintySnapshot result;
    result.level                  = level;
    result.aggregate_score        = agg;
    result.dimensions             = dims;
    result.recommended_action     = action;
    result.size_multiplier        = size_mult;
    result.threshold_multiplier   = threshold_mult;
    result.explanation            = explanation.str();
    result.computed_at            = now;
    result.symbol                 = features.symbol;
    result.top_drivers            = std::move(drivers);
    result.execution_mode         = exec_mode;
    result.cooldown               = cooldown;
    result.freshness_ns           = freshness;
    result.calibration_confidence = cal_conf;
    result.model_version          = 1;
    result.persistent_score       = persistent;
    result.spike_score            = spike;

    // 16. Update state
    update_state(state, agg, level, now_ns);

    // 17. Update diagnostics & 18. Cache snapshot
    {
        std::lock_guard lock(mutex_);
        diagnostics_.total_assessments++;
        if (action == UncertaintyAction::NoTrade) {
            diagnostics_.veto_count++;
        }
        if (cooldown.active) {
            diagnostics_.cooldown_activations++;
        }
        double n = static_cast<double>(diagnostics_.total_assessments);
        diagnostics_.avg_aggregate_score =
            diagnostics_.avg_aggregate_score * ((n - 1.0) / n) + agg / n;
        diagnostics_.last_assessment = now;
        diagnostics_.active_model_version = 1;

        snapshots_[features.symbol.get()] = result;
    }

    // 19. Log
    logger_->debug("Uncertainty",
                   "Неопределённость: " + to_string(action) +
                   " score=" + std::to_string(agg),
                   {{"level",      std::to_string(static_cast<int>(level))},
                    {"aggregate",  std::to_string(agg)},
                    {"action",     to_string(action)},
                    {"exec_mode",  to_string(exec_mode)},
                    {"persistent", std::to_string(persistent)},
                    {"spike",      std::to_string(spike)}});

    // 20. Return
    return result;
}

// ============================================================
// Queries
// ============================================================

std::optional<UncertaintySnapshot> RuleBasedUncertaintyEngine::current(
    const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = snapshots_.find(symbol.get());
    if (it != snapshots_.end()) {
        return it->second;
    }
    return std::nullopt;
}

UncertaintyDiagnostics RuleBasedUncertaintyEngine::diagnostics() const {
    std::lock_guard lock(mutex_);
    return diagnostics_;
}

// ============================================================
// Feedback / Lifecycle
// ============================================================

void RuleBasedUncertaintyEngine::record_feedback(
    const UncertaintyFeedback& feedback) {
    std::lock_guard lock(mutex_);
    feedback_buffer_.push_back(feedback);
    diagnostics_.feedback_samples =
        static_cast<uint32_t>(feedback_buffer_.size());
    diagnostics_.last_feedback = feedback.trade_time;
}

void RuleBasedUncertaintyEngine::reset_state() {
    std::lock_guard lock(mutex_);
    snapshots_.clear();
    states_.clear();
    feedback_buffer_.clear();
    diagnostics_ = UncertaintyDiagnostics{};
}

// ============================================================
// Dimension computors — v1 dimensions (identical logic)
// ============================================================

double RuleBasedUncertaintyEngine::compute_regime_uncertainty(
    const regime::RegimeSnapshot& regime) const {
    return std::clamp(1.0 - regime.confidence, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_signal_uncertainty(
    const features::FeatureSnapshot& features) const {

    const auto& tech = features.technical;
    double uncertainty = 0.3;

    // Конфликт RSI и MACD
    if (tech.rsi_valid && tech.macd_valid) {
        bool rsi_overbought = tech.rsi_14 > 70.0;
        bool rsi_oversold   = tech.rsi_14 < 30.0;
        bool macd_positive  = tech.macd_histogram > 0.0;
        bool macd_negative  = tech.macd_histogram < 0.0;

        if ((rsi_overbought && macd_positive) || (rsi_oversold && macd_negative)) {
            uncertainty -= 0.15;
        }
        if ((rsi_overbought && macd_negative) || (rsi_oversold && macd_positive)) {
            uncertainty += 0.2;
        }
    }

    // Конфликт EMA тренда и RSI
    if (tech.ema_valid && tech.rsi_valid) {
        bool ema_uptrend    = tech.ema_20 > tech.ema_50;
        bool rsi_oversold   = tech.rsi_14 < 30.0;
        bool rsi_overbought = tech.rsi_14 > 70.0;

        if ((ema_uptrend && rsi_overbought) || (!ema_uptrend && rsi_oversold)) {
            uncertainty += 0.1;
        }
    }

    // Мало валидных индикаторов
    int valid_count = 0;
    if (tech.rsi_valid)  ++valid_count;
    if (tech.macd_valid) ++valid_count;
    if (tech.adx_valid)  ++valid_count;
    if (tech.bb_valid)   ++valid_count;
    if (tech.ema_valid)  ++valid_count;

    if (valid_count < 3) {
        uncertainty += 0.2;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_data_quality_uncertainty(
    const features::FeatureSnapshot& features) const {

    double uncertainty = 0.0;

    if (features.book_quality != tb::order_book::BookQuality::Valid) {
        uncertainty += 0.3;
    }
    if (!features.execution_context.is_feed_fresh) {
        uncertainty += 0.3;
    }
    if (features.microstructure.spread_valid &&
        features.microstructure.spread_bps > 30.0) {
        uncertainty += 0.2;
    }
    if (!features.technical.sma_valid) {
        uncertainty += 0.1;
    }
    if (!features.microstructure.spread_valid) {
        uncertainty += 0.1;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_execution_uncertainty(
    const features::FeatureSnapshot& features) const {

    double uncertainty = 0.0;

    if (features.microstructure.spread_valid) {
        uncertainty += std::min(0.4, features.microstructure.spread_bps / 100.0);
    }
    if (features.execution_context.slippage_valid) {
        uncertainty += std::min(0.3,
            features.execution_context.estimated_slippage_bps / 50.0);
    }
    if (features.microstructure.liquidity_valid &&
        features.microstructure.liquidity_ratio > 3.0) {
        uncertainty += 0.2;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

// ============================================================
// Dimension computors — v2 dimensions (NEW)
// ============================================================

double RuleBasedUncertaintyEngine::compute_portfolio_uncertainty(
    const portfolio::PortfolioSnapshot& portfolio) const {

    if (portfolio.positions.empty()) {
        return 0.0;
    }

    double base = 0.0;

    // Concentration risk: single position > 40% or > 60% of capital
    if (portfolio.total_capital > 0.0) {
        for (const auto& pos : portfolio.positions) {
            double pct = std::abs(pos.notional.get()) / portfolio.total_capital;
            if (pct > 0.6) {
                base += 0.5;
                break;
            } else if (pct > 0.4) {
                base += 0.3;
                break;
            }
        }
    }

    // Exposure crowding (capital_utilization_pct is 0–100 %)
    if (portfolio.capital_utilization_pct > 90.0) {
        base += 0.4;
    } else if (portfolio.capital_utilization_pct > 70.0) {
        base += 0.2;
    }

    // Drawdown: daily realized P&L as fraction of capital
    if (portfolio.total_capital > 0.0) {
        double daily_pnl_pct =
            portfolio.pnl.realized_pnl_today / portfolio.total_capital;
        if (daily_pnl_pct < -0.02) {
            base += 0.4;
        } else if (daily_pnl_pct < -0.01) {
            base += 0.2;
        }
    }

    // Many concurrent positions
    if (static_cast<int>(portfolio.positions.size()) > 4) {
        base += 0.1;
    }

    return std::clamp(base, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_ml_uncertainty(
    const ml::MlSignalSnapshot& ml_signals) const {

    if (ml_signals.overall_health == ml::MlComponentHealth::Failed ||
        ml_signals.overall_health == ml::MlComponentHealth::Stale) {
        return 0.7;
    }

    double base = 1.0 - ml_signals.signal_quality;

    if (ml_signals.cascade_imminent) {
        base += 0.3;
    } else {
        base += ml_signals.cascade_probability * 0.3;
    }

    if (ml_signals.recommended_wait_periods < 0) {
        base += 0.2;
    }

    return std::clamp(base, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_correlation_uncertainty(
    const ml::MlSignalSnapshot& ml_signals) const {

    if (ml_signals.correlation_break) {
        return 0.8;
    }

    double base = 1.0 - ml_signals.correlation_risk_multiplier;
    return std::clamp(base, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_transition_uncertainty(
    const regime::RegimeSnapshot& regime) const {

    double base = 0.0;
    if (regime.last_transition.has_value()) {
        base = regime.last_transition->confidence;
    }

    if (regime.stability < 0.3) {
        base += 0.2;
    }

    return std::clamp(base, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_operational_uncertainty(
    const features::FeatureSnapshot& features) const {

    if (!features.execution_context.is_feed_fresh) {
        return 0.7;
    }

    double base = 0.0;

    if (features.book_quality != tb::order_book::BookQuality::Valid) {
        base += 0.3;
    }
    if (features.execution_context.slippage_valid &&
        features.execution_context.estimated_slippage_bps > 20.0) {
        base += 0.2;
    }

    return std::clamp(base, 0.0, 1.0);
}

// ============================================================
// aggregate() — regime-adjusted weighted mean + tail stress
// ============================================================

double RuleBasedUncertaintyEngine::aggregate(
    const UncertaintyDimensions& dims,
    const regime::RegimeSnapshot& regime) const {

    double w_regime      = config_.w_regime;
    double w_signal      = config_.w_signal;
    double w_data        = config_.w_data_quality;
    double w_execution   = config_.w_execution;
    double w_portfolio   = config_.w_portfolio;
    double w_ml          = config_.w_ml;
    double w_correlation = config_.w_correlation;
    double w_transition  = config_.w_transition;
    double w_operational = config_.w_operational;

    // Regime-specific amplifiers
    if (regime.label == RegimeLabel::Volatile) {
        w_data      *= 1.5;
        w_execution *= 1.5;
        w_signal    *= 0.8;
    } else if (regime.label == RegimeLabel::Unclear) {
        w_regime      *= 1.2;
        w_signal      *= 1.2;
        w_data        *= 1.2;
        w_execution   *= 1.2;
        w_portfolio   *= 1.2;
        w_ml          *= 1.2;
        w_correlation *= 1.2;
        w_transition  *= 1.2;
        w_operational *= 1.2;
    }

    // Normalize
    double total_w = w_regime + w_signal + w_data + w_execution + w_portfolio +
                     w_ml + w_correlation + w_transition + w_operational;
    if (total_w <= 0.0) {
        return 0.5;
    }

    double result = (w_regime      * dims.regime_uncertainty       +
                     w_signal      * dims.signal_uncertainty       +
                     w_data        * dims.data_quality_uncertainty +
                     w_execution   * dims.execution_uncertainty    +
                     w_portfolio   * dims.portfolio_uncertainty    +
                     w_ml          * dims.ml_uncertainty           +
                     w_correlation * dims.correlation_uncertainty  +
                     w_transition  * dims.transition_uncertainty   +
                     w_operational * dims.operational_uncertainty) / total_w;

    // Tail-stress amplifier: each dimension > 0.8 adds 0.05
    const double raw_dims[] = {
        dims.regime_uncertainty,       dims.signal_uncertainty,
        dims.data_quality_uncertainty, dims.execution_uncertainty,
        dims.portfolio_uncertainty,    dims.ml_uncertainty,
        dims.correlation_uncertainty,  dims.transition_uncertainty,
        dims.operational_uncertainty
    };
    for (double d : raw_dims) {
        if (d > 0.8) {
            result += 0.05;
        }
    }

    return std::clamp(result, 0.0, 1.0);
}

// ============================================================
// apply_hysteresis() — prevents oscillation around thresholds
// ============================================================

UncertaintyLevel RuleBasedUncertaintyEngine::apply_hysteresis(
    double score, const SymbolState& state) const {

    auto prev = state.prev_level;

    // Raw level without hysteresis
    UncertaintyLevel raw_level;
    if (score < config_.threshold_low) {
        raw_level = UncertaintyLevel::Low;
    } else if (score < config_.threshold_moderate) {
        raw_level = UncertaintyLevel::Moderate;
    } else if (score < config_.threshold_high) {
        raw_level = UncertaintyLevel::High;
    } else {
        raw_level = UncertaintyLevel::Extreme;
    }

    if (raw_level == prev) {
        return prev;
    }

    // Raising level: need score >= threshold + hysteresis_up
    if (static_cast<int>(raw_level) > static_cast<int>(prev)) {
        double threshold = 0.0;
        switch (raw_level) {
            case UncertaintyLevel::Moderate: threshold = config_.threshold_low;      break;
            case UncertaintyLevel::High:     threshold = config_.threshold_moderate;  break;
            case UncertaintyLevel::Extreme:  threshold = config_.threshold_high;      break;
            default: break;
        }
        if (score >= threshold + config_.hysteresis_up) {
            return raw_level;
        }
        return prev;
    }

    // Lowering level: need score < threshold - hysteresis_down
    double threshold = 0.0;
    switch (prev) {
        case UncertaintyLevel::Moderate: threshold = config_.threshold_low;      break;
        case UncertaintyLevel::High:     threshold = config_.threshold_moderate;  break;
        case UncertaintyLevel::Extreme:  threshold = config_.threshold_high;      break;
        default: break;
    }
    if (score < threshold - config_.hysteresis_down) {
        return raw_level;
    }
    return prev;
}

// ============================================================
// update_state() — per-symbol tracking after each assessment
// ============================================================

void RuleBasedUncertaintyEngine::update_state(
    SymbolState& state, double raw_score,
    UncertaintyLevel new_level, int64_t now_ns) {

    state.ema_score =
        config_.ema_alpha * raw_score +
        (1.0 - config_.ema_alpha) * state.ema_score;

    state.peak_score = std::max(state.peak_score * 0.95, raw_score);

    state.shock_memory = std::max(
        state.shock_memory * 0.98,
        std::max(0.0, raw_score - state.ema_score));

    if (state.prev_level != new_level) {
        state.last_level_change_ns = now_ns;
    }

    state.prev_level    = new_level;
    state.last_assess_ns = now_ns;

    if (new_level == UncertaintyLevel::Extreme) {
        state.consecutive_extreme++;
    } else {
        state.consecutive_extreme = 0;
    }

    if (new_level == UncertaintyLevel::High ||
        new_level == UncertaintyLevel::Extreme) {
        state.consecutive_high++;
    } else {
        state.consecutive_high = 0;
    }

    // 3+ consecutive extreme → activate 60-second cooldown
    if (state.consecutive_extreme >= 3) {
        constexpr int64_t sixty_seconds_ns = 60'000'000'000LL;
        state.cooldown_until_ns = now_ns + sixty_seconds_ns;
    }
}

// ============================================================
// compute_cooldown()
// ============================================================

CooldownRecommendation RuleBasedUncertaintyEngine::compute_cooldown(
    const SymbolState& state, int64_t now_ns) const {

    CooldownRecommendation cd;

    if (now_ns < state.cooldown_until_ns) {
        cd.active       = true;
        cd.remaining_ns = state.cooldown_until_ns - now_ns;
        constexpr int64_t max_cooldown_ns = 60'000'000'000LL;
        double ratio = static_cast<double>(cd.remaining_ns) /
                       static_cast<double>(max_cooldown_ns);
        cd.decay_factor    = 0.5 + 0.5 * std::clamp(ratio, 0.0, 1.0);
        cd.trigger_reason  = "Серия экстремальных оценок (≥3 подряд)";
    } else {
        cd.active       = false;
        cd.remaining_ns = 0;
        cd.decay_factor = 1.0;
    }

    return cd;
}

// ============================================================
// determine_execution_mode()
// ============================================================

ExecutionModeRecommendation RuleBasedUncertaintyEngine::determine_execution_mode(
    UncertaintyLevel level, double score, const SymbolState& state) const {

    if (level == UncertaintyLevel::Extreme) {
        return ExecutionModeRecommendation::HaltNewEntries;
    }
    if (level == UncertaintyLevel::High && state.consecutive_high >= 3) {
        return ExecutionModeRecommendation::DefensiveOnly;
    }
    if (level == UncertaintyLevel::High) {
        return ExecutionModeRecommendation::Conservative;
    }
    if (score > 0.4) {
        return ExecutionModeRecommendation::Conservative;
    }
    return ExecutionModeRecommendation::Normal;
}

// ============================================================
// compute_drivers() — top-3 uncertainty drivers
// ============================================================

std::vector<UncertaintyDriver> RuleBasedUncertaintyEngine::compute_drivers(
    const UncertaintyDimensions& dims) const {

    struct DimEntry {
        const char* name;
        double weight;
        double raw;
        const char* desc;
    };

    const DimEntry entries[] = {
        {"regime_uncertainty",       config_.w_regime,
         dims.regime_uncertainty,
         "Высокая неопределённость режима"},
        {"signal_uncertainty",       config_.w_signal,
         dims.signal_uncertainty,
         "Конфликтующие торговые сигналы"},
        {"data_quality_uncertainty", config_.w_data_quality,
         dims.data_quality_uncertainty,
         "Низкое качество данных"},
        {"execution_uncertainty",    config_.w_execution,
         dims.execution_uncertainty,
         "Высокая неопределённость исполнения"},
        {"portfolio_uncertainty",    config_.w_portfolio,
         dims.portfolio_uncertainty,
         "Портфельный риск (концентрация/просадка)"},
        {"ml_uncertainty",           config_.w_ml,
         dims.ml_uncertainty,
         "Деградация ML-моделей"},
        {"correlation_uncertainty",  config_.w_correlation,
         dims.correlation_uncertainty,
         "Нарушение корреляционной структуры"},
        {"transition_uncertainty",   config_.w_transition,
         dims.transition_uncertainty,
         "Нестабильность: вероятный переход режима"},
        {"operational_uncertainty",  config_.w_operational,
         dims.operational_uncertainty,
         "Операционные проблемы (фид, инфраструктура)"},
    };

    std::vector<UncertaintyDriver> drivers;
    drivers.reserve(9);
    for (const auto& e : entries) {
        UncertaintyDriver d;
        d.dimension    = e.name;
        d.contribution = e.weight * e.raw;
        d.raw_value    = e.raw;
        d.description  = e.desc;
        drivers.push_back(std::move(d));
    }

    std::sort(drivers.begin(), drivers.end(),
              [](const UncertaintyDriver& a, const UncertaintyDriver& b) {
                  return a.contribution > b.contribution;
              });

    if (drivers.size() > 3) {
        drivers.resize(3);
    }
    return drivers;
}

} // namespace tb::uncertainty
