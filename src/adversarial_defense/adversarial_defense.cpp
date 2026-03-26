/**
 * @file adversarial_defense.cpp
 * @brief Реализация защиты от враждебных рыночных условий
 */

#include "adversarial_defense.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace tb::adversarial {

namespace {

constexpr int64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr int64_t kCooldownCleanupIntervalMs = 60'000;

[[nodiscard]] int64_t to_milliseconds(Timestamp ts) {
    return ts.get() <= 0 ? 0 : ts.get() / kNanosecondsPerMillisecond;
}

[[nodiscard]] double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] bool is_finite(double value) {
    return std::isfinite(value);
}

[[nodiscard]] std::string format_double(double value, int precision = 4) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, value);
    return std::string(buf);
}

[[nodiscard]] int action_priority(DefenseAction action) {
    switch (action) {
        case DefenseAction::NoAction:         return 0;
        case DefenseAction::ReduceConfidence: return 1;
        case DefenseAction::RaiseThreshold:   return 2;
        case DefenseAction::AlertOperator:    return 3;
        case DefenseAction::Cooldown:         return 4;
        case DefenseAction::VetoTrade:        return 5;
    }
    return 0;
}

[[nodiscard]] DefenseAction stronger_action(DefenseAction current, DefenseAction candidate) {
    return action_priority(candidate) > action_priority(current) ? candidate : current;
}

/// Определить action по severity (общий паттерн для детекторов)
[[nodiscard]] DefenseAction severity_to_action(double severity) {
    if (severity >= 0.85) return DefenseAction::VetoTrade;
    if (severity >= 0.4)  return DefenseAction::RaiseThreshold;
    return DefenseAction::ReduceConfidence;
}

/// Версия без VetoTrade — для некритичных детекторов (микро-кап нормы)
[[nodiscard]] DefenseAction severity_to_action_soft(double severity) {
    if (severity >= 0.4) return DefenseAction::RaiseThreshold;
    return DefenseAction::ReduceConfidence;
}

void validate_config(const DefenseConfig& config) {
    if (config.spread_explosion_threshold_bps <= 0.0) {
        throw std::invalid_argument("spread_explosion_threshold_bps must be > 0");
    }
    if (config.spread_normal_bps <= 0.0) {
        throw std::invalid_argument("spread_normal_bps must be > 0");
    }
    if (config.min_liquidity_depth <= 0.0) {
        throw std::invalid_argument("min_liquidity_depth must be > 0");
    }
    if (config.book_imbalance_threshold <= 0.0 || config.book_imbalance_threshold > 1.0) {
        throw std::invalid_argument("book_imbalance_threshold must be in (0, 1]");
    }
    if (config.book_instability_threshold < 0.0 || config.book_instability_threshold >= 1.0) {
        throw std::invalid_argument("book_instability_threshold must be in [0, 1)");
    }
    if (config.toxic_flow_ratio_threshold <= 1.0) {
        throw std::invalid_argument("toxic_flow_ratio_threshold must be > 1.0");
    }
    if (config.aggressive_flow_threshold <= 0.5 || config.aggressive_flow_threshold >= 1.0) {
        throw std::invalid_argument("aggressive_flow_threshold must be in (0.5, 1.0)");
    }
    if (config.vpin_toxic_threshold <= 0.0 || config.vpin_toxic_threshold > 1.0) {
        throw std::invalid_argument("vpin_toxic_threshold must be in (0, 1]");
    }
    if (config.cooldown_duration_ms <= 0 || config.post_shock_cooldown_ms <= 0) {
        throw std::invalid_argument("cooldown durations must be > 0");
    }
    if (config.max_market_data_age_ns <= 0) {
        throw std::invalid_argument("max_market_data_age_ns must be > 0");
    }
    if (config.auto_cooldown_severity < 0.0 || config.auto_cooldown_severity > 1.0) {
        throw std::invalid_argument("auto_cooldown_severity must be in [0, 1]");
    }
    if (config.max_confidence_reduction < 0.0 || config.max_confidence_reduction > 1.0) {
        throw std::invalid_argument("max_confidence_reduction must be in [0, 1]");
    }
    if (config.max_threshold_expansion < 0.0) {
        throw std::invalid_argument("max_threshold_expansion must be >= 0");
    }
    if (config.compound_threat_factor < 0.0 || config.compound_threat_factor > 1.0) {
        throw std::invalid_argument("compound_threat_factor must be in [0, 1]");
    }
    if (config.cooldown_severity_scale < 1.0) {
        throw std::invalid_argument("cooldown_severity_scale must be >= 1.0");
    }
    if (config.recovery_duration_ms < 0) {
        throw std::invalid_argument("recovery_duration_ms must be >= 0");
    }
    if (config.recovery_confidence_floor < 0.0 || config.recovery_confidence_floor > 1.0) {
        throw std::invalid_argument("recovery_confidence_floor must be in [0, 1]");
    }
    if (config.spread_velocity_threshold_bps_per_sec <= 0.0) {
        throw std::invalid_argument("spread_velocity_threshold_bps_per_sec must be > 0");
    }
    // --- Adaptive baseline ---
    if (config.baseline_alpha <= 0.0 || config.baseline_alpha >= 1.0) {
        throw std::invalid_argument("baseline_alpha must be in (0, 1)");
    }
    if (config.baseline_warmup_ticks < 1) {
        throw std::invalid_argument("baseline_warmup_ticks must be >= 1");
    }
    if (config.z_score_spread_threshold <= 0.0) {
        throw std::invalid_argument("z_score_spread_threshold must be > 0");
    }
    if (config.z_score_depth_threshold <= 0.0) {
        throw std::invalid_argument("z_score_depth_threshold must be > 0");
    }
    if (config.z_score_ratio_threshold <= 0.0) {
        throw std::invalid_argument("z_score_ratio_threshold must be > 0");
    }
    if (config.baseline_stale_reset_ms <= 0) {
        throw std::invalid_argument("baseline_stale_reset_ms must be > 0");
    }
    // --- Threat memory ---
    if (config.threat_memory_alpha <= 0.0 || config.threat_memory_alpha >= 1.0) {
        throw std::invalid_argument("threat_memory_alpha must be in (0, 1)");
    }
    if (config.threat_memory_residual_factor < 0.0 || config.threat_memory_residual_factor > 1.0) {
        throw std::invalid_argument("threat_memory_residual_factor must be in [0, 1]");
    }
    if (config.threat_escalation_ticks < 1) {
        throw std::invalid_argument("threat_escalation_ticks must be >= 1");
    }
    if (config.threat_escalation_boost < 0.0) {
        throw std::invalid_argument("threat_escalation_boost must be >= 0");
    }
    // --- Depth asymmetry ---
    if (config.depth_asymmetry_threshold <= 0.0 || config.depth_asymmetry_threshold >= 1.0) {
        throw std::invalid_argument("depth_asymmetry_threshold must be in (0, 1)");
    }
    // --- Cross-signal amplification ---
    if (config.cross_signal_amplification < 0.0) {
        throw std::invalid_argument("cross_signal_amplification must be >= 0");
    }
    // --- v4: Percentile scoring ---
    if (config.percentile_window_size < 10) throw std::invalid_argument("percentile_window_size must be >= 10");
    if (config.percentile_severity_threshold <= 0.0 || config.percentile_severity_threshold >= 1.0)
        throw std::invalid_argument("percentile_severity_threshold must be in (0, 1)");
    // --- v4: Correlation matrix ---
    if (config.correlation_alpha <= 0.0 || config.correlation_alpha >= 1.0)
        throw std::invalid_argument("correlation_alpha must be in (0, 1)");
    if (config.correlation_breakdown_threshold <= 0.0)
        throw std::invalid_argument("correlation_breakdown_threshold must be > 0");
    // --- v4: Multi-timeframe ---
    if (config.baseline_halflife_fast_ms <= 0.0) throw std::invalid_argument("baseline_halflife_fast_ms must be > 0");
    if (config.baseline_halflife_medium_ms <= config.baseline_halflife_fast_ms)
        throw std::invalid_argument("baseline_halflife_medium_ms must be > fast");
    if (config.baseline_halflife_slow_ms <= config.baseline_halflife_medium_ms)
        throw std::invalid_argument("baseline_halflife_slow_ms must be > medium");
    if (config.timeframe_divergence_threshold <= 0.0)
        throw std::invalid_argument("timeframe_divergence_threshold must be > 0");
    // --- v4: Hysteresis ---
    if (config.hysteresis_enter_severity <= 0.0 || config.hysteresis_enter_severity >= 1.0)
        throw std::invalid_argument("hysteresis_enter_severity must be in (0, 1)");
    if (config.hysteresis_exit_severity < 0.0 || config.hysteresis_exit_severity >= config.hysteresis_enter_severity)
        throw std::invalid_argument("hysteresis_exit_severity must be in [0, enter_severity)");
    if (config.hysteresis_confidence_penalty < 0.0 || config.hysteresis_confidence_penalty > 1.0)
        throw std::invalid_argument("hysteresis_confidence_penalty must be in [0, 1]");
    // --- v4: Event sourcing ---
    if (config.audit_log_max_size < 0) throw std::invalid_argument("audit_log_max_size must be >= 0");
}

} // namespace

AdversarialMarketDefense::AdversarialMarketDefense(DefenseConfig config)
    : config_(std::move(config)) {
    validate_config(config_);
}

DefenseAssessment AdversarialMarketDefense::assess(const MarketCondition& condition) {
    std::lock_guard<std::mutex> lock(mutex_);

    DefenseAssessment result;
    result.symbol = condition.symbol;
    result.assessed_at = condition.timestamp;
    const auto sym_key = condition.symbol.get();

    if (!config_.enabled) {
        return result;
    }

    cleanup_expired_cooldowns_locked(condition.timestamp);

    if (auto threat = detect_invalid_market_state(condition)) {
        result.threats.push_back(*threat);
    }

    // 1. Проверяем cooldown — если активен, торговля запрещена
    auto cooldown_threat = check_cooldown(condition.symbol, condition.timestamp);
    if (cooldown_threat.recommended_action != DefenseAction::NoAction) {
        result.threats.push_back(cooldown_threat);
        result.cooldown_active = true;
        result.cooldown_remaining_ms = cooldown_remaining_ms_locked(condition.symbol, condition.timestamp);
    }

    const bool input_invalid = std::any_of(
        result.threats.begin(),
        result.threats.end(),
        [](const ThreatDetection& threat) {
            return threat.type == ThreatType::InvalidMarketState;
        });

    if (!input_invalid) {
        if (auto threat = detect_stale_market_data(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_spread_explosion(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_spread_velocity(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_liquidity_vacuum(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_unstable_book(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_toxic_flow(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_bad_breakout(condition)) {
            result.threats.push_back(*threat);
        }
        // --- Новые детекторы ---
        if (auto threat = detect_depth_asymmetry(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_anomalous_baseline(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_correlation_breakdown(condition)) {
            result.threats.push_back(*threat);
        }
        if (auto threat = detect_timeframe_divergence(condition)) {
            result.threats.push_back(*threat);
        }
    }

    // Threat escalation (uses memory state from previous ticks, runs even if no current threats)
    {
        const bool has_current = std::any_of(
            result.threats.begin(), result.threats.end(),
            [](const ThreatDetection& t) {
                return t.severity > 0.0 &&
                       t.type != ThreatType::PostShockCooldown &&
                       t.type != ThreatType::InvalidMarketState;
            });
        if (auto threat = detect_threat_escalation(condition, has_current)) {
            result.threats.push_back(*threat);
        }
    }

    // Вычислить compound severity вероятностной моделью
    double compound_severity = compute_compound_severity(
        result.threats, config_.compound_threat_factor);

    // Cross-signal amplification: опасные комбинации усиливают severity
    compound_severity = apply_cross_signal_amplification(result.threats, compound_severity);
    result.compound_severity = compound_severity;

    // Percentile-based severity overlay
    const double pct_severity = compute_percentile_severity_locked(sym_key, condition);
    result.percentile_severity = pct_severity;
    if (pct_severity > 0.0) {
        compound_severity = std::max(compound_severity, compound_severity * 0.7 + pct_severity * 0.3);
        result.compound_severity = compound_severity;
    }

    // Классификация рыночного режима
    result.regime = classify_regime(condition, compound_severity, result.threats);

    bool should_register_cooldown = false;
    for (const auto& t : result.threats) {
        result.overall_action = stronger_action(result.overall_action, t.recommended_action);

        if (!result.cooldown_active &&
            config_.auto_cooldown_on_veto &&
            t.type != ThreatType::PostShockCooldown &&
            t.recommended_action == DefenseAction::VetoTrade &&
            t.severity >= config_.auto_cooldown_severity) {
            should_register_cooldown = true;
        }
    }

    result.is_safe = (result.overall_action != DefenseAction::VetoTrade &&
                      result.overall_action != DefenseAction::Cooldown);

    // Нелинейные кривые confidence/threshold от compound severity
    if (!result.threats.empty()) {
        const double sev2 = compound_severity * compound_severity; // quadratic
        result.confidence_multiplier = std::max(
            0.0,
            1.0 - sev2 * config_.max_confidence_reduction);
        const double sev15 = compound_severity * std::sqrt(compound_severity); // severity^1.5
        result.threshold_multiplier = 1.0 + sev15 * config_.max_threshold_expansion;
    }

    // Post-cooldown recovery: снижаем confidence даже после окончания cooldown
    const double recovery_mult = compute_recovery_multiplier_locked(
        condition.symbol, condition.timestamp);
    if (recovery_mult < 1.0) {
        result.in_recovery = true;
        result.confidence_multiplier = std::min(
            result.confidence_multiplier, recovery_mult);
    }

    // Hysteresis: предотвращает chattering на границе safe/unsafe
    result.hysteresis_active = update_hysteresis_locked(sym_key, compound_severity);
    if (result.hysteresis_active && result.is_safe) {
        result.confidence_multiplier = std::max(
            0.0, result.confidence_multiplier - config_.hysteresis_confidence_penalty);
    }

    // Threat memory: residual confidence reduction при отсутствии текущих угроз
    // (reads memory from PREVIOUS ticks — updated below)
    {
        auto mem_it = threat_memories_.find(sym_key);
        if (mem_it != threat_memories_.end()) {
            // Если compound_severity низкий но memory elevated — снижаем confidence
            if (compound_severity < 0.2 && mem_it->second.ema_severity > 0.1) {
                double residual = mem_it->second.ema_severity
                                  * config_.threat_memory_residual_factor;
                result.confidence_multiplier = std::max(
                    0.0,
                    std::min(result.confidence_multiplier, 1.0 - residual));
            }
        }
    }

    // Populate baseline status
    {
        auto bl_it = baselines_.find(sym_key);
        result.baseline_warm = (bl_it != baselines_.end()) &&
                               bl_it->second.is_warm(config_.baseline_warmup_ticks);
    }

    // Severity-proportional cooldown: duration * (1 + (severity - threshold) * scale)
    if (should_register_cooldown && !condition.symbol.get().empty()) {
        const double max_sev = compound_severity;
        const double scale_factor = 1.0 + (max_sev - config_.auto_cooldown_severity)
                                          * config_.cooldown_severity_scale;
        const auto cd_duration = static_cast<int64_t>(
            config_.post_shock_cooldown_ms * std::clamp(scale_factor, 1.0, 3.0));
        const int64_t now_ms = to_milliseconds(condition.timestamp);
        cooldown_until_[condition.symbol.get()] = now_ms + cd_duration;
        if (config_.recovery_duration_ms > 0) {
            recovery_until_[condition.symbol.get()] = now_ms + cd_duration
                                                      + config_.recovery_duration_ms;
        }
    }

    update_symbol_state_locked(condition);

    // Обновить adaptive baseline и threat memory (используют данные текущего тика)
    if (!input_invalid) {
        update_baseline_locked(condition);
    }
    {
        const bool has_active_threats = std::any_of(
            result.threats.begin(), result.threats.end(),
            [](const ThreatDetection& t) {
                return t.severity > 0.0 &&
                       t.type != ThreatType::PostShockCooldown;
            });
        update_threat_memory_locked(
            sym_key, compound_severity, has_active_threats,
            to_milliseconds(condition.timestamp));
    }

    // Populate threat memory severity AFTER update (reflects current tick)
    {
        auto mem_it = threat_memories_.find(sym_key);
        if (mem_it != threat_memories_.end()) {
            result.threat_memory_severity = mem_it->second.ema_severity;
        }
    }

    // Update state trackers (must be after all detection)
    if (!input_invalid) {
        update_percentile_window_locked(condition);
        update_correlation_locked(condition);
        update_mtf_baselines_locked(condition);
    }

    // Event sourcing & calibration
    emit_event_locked(result, to_milliseconds(condition.timestamp));
    update_calibration_locked(result, result.threats);

    return result;
}

void AdversarialMarketDefense::register_shock(Symbol symbol, ThreatType type, Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t now_ms = to_milliseconds(now);
    int64_t duration = (type == ThreatType::PostShockCooldown)
        ? config_.post_shock_cooldown_ms
        : config_.cooldown_duration_ms;

    cooldown_until_[symbol.get()] = now_ms + duration;
    if (config_.recovery_duration_ms > 0) {
        recovery_until_[symbol.get()] = now_ms + duration + config_.recovery_duration_ms;
    }
}

bool AdversarialMarketDefense::is_cooldown_active(const Symbol& symbol, Timestamp now) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cooldown_until_.find(symbol.get());
    if (it == cooldown_until_.end()) return false;

    int64_t now_ms = to_milliseconds(now);
    return now_ms < it->second;
}

void AdversarialMarketDefense::reset_cooldown(const Symbol& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    cooldown_until_.erase(symbol.get());
    recovery_until_.erase(symbol.get());
}

// --- Детекторы угроз ---

std::optional<ThreatDetection> AdversarialMarketDefense::detect_invalid_market_state(
    const MarketCondition& c) const {

    if (c.symbol.get().empty()) {
        return ThreatDetection{
            .type = ThreatType::InvalidMarketState,
            .severity = 1.0,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = "Пустой symbol в MarketCondition",
            .detected_at = c.timestamp
        };
    }

    if (c.timestamp.get() <= 0) {
        return ThreatDetection{
            .type = ThreatType::InvalidMarketState,
            .severity = 1.0,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = "Некорректный timestamp в MarketCondition",
            .detected_at = c.timestamp
        };
    }

    const bool malformed =
        !is_finite(c.spread_bps) ||
        !is_finite(c.book_imbalance) ||
        !is_finite(c.bid_depth) ||
        !is_finite(c.ask_depth) ||
        !is_finite(c.book_instability) ||
        !is_finite(c.buy_sell_ratio) ||
        !is_finite(c.aggressive_flow) ||
        !is_finite(c.vpin) ||
        c.spread_bps < 0.0 ||
        c.bid_depth < 0.0 ||
        c.ask_depth < 0.0 ||
        c.book_instability < 0.0 ||
        c.book_instability > 1.0 ||
        std::abs(c.book_imbalance) > 1.0 ||
        c.buy_sell_ratio < 0.0 ||
        c.aggressive_flow < 0.0 ||
        c.aggressive_flow > 1.0 ||
        c.vpin < 0.0 ||
        c.vpin > 1.0 ||
        c.market_data_age_ns < 0;

    if (malformed) {
        return ThreatDetection{
            .type = ThreatType::InvalidMarketState,
            .severity = 1.0,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = "Некорректные или нефинитные входные рыночные данные",
            .detected_at = c.timestamp
        };
    }

    if (config_.fail_closed_on_invalid_data &&
        (!c.spread_valid || !c.liquidity_valid || !c.instability_valid)) {
        return ThreatDetection{
            .type = ThreatType::InvalidMarketState,
            .severity = 0.9,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = "Критические микроструктурные признаки недоступны",
            .detected_at = c.timestamp
        };
    }

    return std::nullopt;
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_stale_market_data(
    const MarketCondition& c) const {

    if (c.market_data_fresh && c.market_data_age_ns <= config_.max_market_data_age_ns) {
        return std::nullopt;
    }

    // Severity: 0.5 в точке threshold, 1.0 на 2x threshold
    const double age_ratio = c.market_data_age_ns > 0
        ? static_cast<double>(c.market_data_age_ns) /
          static_cast<double>(config_.max_market_data_age_ns * 2)
        : 1.0;
    const double severity = std::max(0.5, clamp01(age_ratio + 0.5));

    return ThreatDetection{
        .type = ThreatType::StaleMarketData,
        .severity = severity,
        .recommended_action = severity_to_action(severity),
        .reason = "Market data stale: age_ns=" + std::to_string(c.market_data_age_ns),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_spread_explosion(
    const MarketCondition& c) const {

    if (!c.spread_valid || c.spread_bps <= config_.spread_explosion_threshold_bps) {
        return std::nullopt;
    }

    const double severity = clamp01(
        (c.spread_bps - config_.spread_explosion_threshold_bps) /
        config_.spread_explosion_threshold_bps);

    return ThreatDetection{
        .type = ThreatType::SpreadExplosion,
        .severity = severity,
        .recommended_action = severity_to_action(severity),
        .reason = "Спред " + format_double(c.spread_bps) +
                  " bps превышает порог " + format_double(config_.spread_explosion_threshold_bps),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_spread_velocity(
    const MarketCondition& c) const {

    if (!c.spread_valid) {
        return std::nullopt;
    }

    const auto sym_key = c.symbol.get();
    auto it = symbol_tick_state_.find(sym_key);
    if (it == symbol_tick_state_.end()) {
        return std::nullopt; // нет предыдущего тика — пропуск
    }

    const auto& prev = it->second;
    const int64_t now_ms = to_milliseconds(c.timestamp);
    const int64_t dt_ms = now_ms - prev.tick_ms;
    if (dt_ms <= 0 || dt_ms > 10000) {
        // Слишком маленький или большой интервал (>10с — данные не релевантны)
        return std::nullopt;
    }

    const double spread_delta = c.spread_bps - prev.spread_bps;
    if (spread_delta <= 0.0) {
        return std::nullopt; // спред сужается — не угроза
    }

    const double velocity_bps_per_sec = spread_delta / (static_cast<double>(dt_ms) / 1000.0);
    if (velocity_bps_per_sec <= config_.spread_velocity_threshold_bps_per_sec) {
        return std::nullopt;
    }

    const double severity = clamp01(
        (velocity_bps_per_sec - config_.spread_velocity_threshold_bps_per_sec) /
        config_.spread_velocity_threshold_bps_per_sec);

    return ThreatDetection{
        .type = ThreatType::SpreadVelocitySpike,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Скорость расширения спреда " + format_double(velocity_bps_per_sec, 1) +
                  " bps/s превышает порог " +
                  format_double(config_.spread_velocity_threshold_bps_per_sec, 1),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_liquidity_vacuum(
    const MarketCondition& c) const {

    if (!c.liquidity_valid) {
        return std::nullopt;
    }

    double min_depth = std::min(c.bid_depth, c.ask_depth);
    if (min_depth >= config_.min_liquidity_depth) return std::nullopt;

    const double ratio = min_depth / config_.min_liquidity_depth;
    const double severity = clamp01(1.0 - ratio);

    return ThreatDetection{
        .type = ThreatType::LiquidityVacuum,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Минимальная глубина " + format_double(min_depth) +
                  " ниже порога " + format_double(config_.min_liquidity_depth),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_unstable_book(
    const MarketCondition& c) const {

    if (!c.book_valid) {
        return ThreatDetection{
            .type = ThreatType::UnstableOrderBook,
            .severity = 1.0,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = c.book_state.empty()
                ? "Стакан невалиден"
                : "Стакан невалиден: " + c.book_state,
            .detected_at = c.timestamp
        };
    }

    if (!c.instability_valid) {
        return std::nullopt;
    }

    if (c.book_instability <= config_.book_instability_threshold) return std::nullopt;

    const double severity = clamp01(
        (c.book_instability - config_.book_instability_threshold) /
        (1.0 - config_.book_instability_threshold));

    return ThreatDetection{
        .type = ThreatType::UnstableOrderBook,
        .severity = severity,
        .recommended_action = DefenseAction::ReduceConfidence,
        .reason = "Нестабильность стакана " + format_double(c.book_instability) +
                   " превышает порог " + format_double(config_.book_instability_threshold),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_toxic_flow(
    const MarketCondition& c) const {

    double ratio_severity = 0.0;
    if (c.flow_valid) {
        // FIX: buy_sell_ratio == 0.0 (нулевой объём покупок = 100% sell pressure)
        // — не пропускаем, обрабатываем как нижний порог
        if (c.buy_sell_ratio >= config_.toxic_flow_ratio_threshold) {
            ratio_severity = clamp01(
                (c.buy_sell_ratio - config_.toxic_flow_ratio_threshold) /
                config_.toxic_flow_ratio_threshold);
        } else {
            const double lower_ratio = 1.0 / config_.toxic_flow_ratio_threshold;
            if (c.buy_sell_ratio <= lower_ratio) {
                ratio_severity = clamp01((lower_ratio - c.buy_sell_ratio) / lower_ratio);
            }
        }
    }

    double aggressive_flow_severity = 0.0;
    if (c.flow_valid) {
        if (c.aggressive_flow >= config_.aggressive_flow_threshold) {
            aggressive_flow_severity = clamp01(
                (c.aggressive_flow - config_.aggressive_flow_threshold) /
                (1.0 - config_.aggressive_flow_threshold));
        } else {
            const double lower_aggressive_threshold = 1.0 - config_.aggressive_flow_threshold;
            if (c.aggressive_flow <= lower_aggressive_threshold) {
                aggressive_flow_severity = clamp01(
                    (lower_aggressive_threshold - c.aggressive_flow) /
                    lower_aggressive_threshold);
            }
        }
    }

    double vpin_severity = 0.0;
    if (c.vpin_valid && c.vpin >= config_.vpin_toxic_threshold) {
        vpin_severity = clamp01(
            (c.vpin - config_.vpin_toxic_threshold) /
            (1.0 - config_.vpin_toxic_threshold));
    }

    const double severity = std::max({ratio_severity, aggressive_flow_severity, vpin_severity});
    if (severity <= 0.0) {
        return std::nullopt;
    }

    return ThreatDetection{
        .type = ThreatType::ToxicFlow,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Токсичный поток: ratio=" + format_double(c.buy_sell_ratio) +
                  ", aggressive=" + format_double(c.aggressive_flow) +
                  ", vpin=" + format_double(c.vpin),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_bad_breakout(
    const MarketCondition& c) const {

    if (!c.spread_valid || !c.imbalance_valid) {
        return std::nullopt;
    }

    const bool spread_expanded = c.spread_bps > config_.spread_normal_bps * 2.0;
    const bool strong_imbalance = std::abs(c.book_imbalance) >= config_.book_imbalance_threshold;

    if (!spread_expanded || !strong_imbalance) return std::nullopt;

    const double severity = clamp01(
        (c.spread_bps / (config_.spread_normal_bps * 4.0)) *
        std::abs(c.book_imbalance));

    return ThreatDetection{
        .type = ThreatType::BadBreakoutTrap,
        .severity = severity * 0.6,
        .recommended_action = DefenseAction::ReduceConfidence,
        .reason = "Возможная ловушка: спред " + format_double(c.spread_bps) +
                  " bps + дисбаланс " + format_double(c.book_imbalance),
        .detected_at = c.timestamp
    };
}

ThreatDetection AdversarialMarketDefense::check_cooldown(
    const Symbol& symbol, Timestamp now) const {

    auto it = cooldown_until_.find(symbol.get());
    if (it == cooldown_until_.end()) {
        return ThreatDetection{
            .type = ThreatType::PostShockCooldown,
            .severity = 0.0,
            .recommended_action = DefenseAction::NoAction,
            .reason = "",
            .detected_at = now
        };
    }

    int64_t now_ms = to_milliseconds(now);
    if (now_ms >= it->second) {
        return ThreatDetection{
            .type = ThreatType::PostShockCooldown,
            .severity = 0.0,
            .recommended_action = DefenseAction::NoAction,
            .reason = "",
            .detected_at = now
        };
    }

    return ThreatDetection{
        .type = ThreatType::PostShockCooldown,
        .severity = 1.0,
        .recommended_action = DefenseAction::Cooldown,
        .reason = "Cooldown активен для " + symbol.get(),
        .detected_at = now
    };
}

int64_t AdversarialMarketDefense::cooldown_remaining_ms_locked(
    const Symbol& symbol, Timestamp now) const {

    const auto it = cooldown_until_.find(symbol.get());
    if (it == cooldown_until_.end()) {
        return 0;
    }

    return std::max(int64_t{0}, it->second - to_milliseconds(now));
}

void AdversarialMarketDefense::cleanup_expired_cooldowns_locked(Timestamp now) {
    const int64_t now_ms = to_milliseconds(now);
    if (now_ms <= 0 || (now_ms - last_cleanup_ms_) < kCooldownCleanupIntervalMs) {
        return;
    }

    for (auto it = cooldown_until_.begin(); it != cooldown_until_.end();) {
        if (it->second <= now_ms) {
            it = cooldown_until_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = recovery_until_.begin(); it != recovery_until_.end();) {
        if (it->second <= now_ms) {
            it = recovery_until_.erase(it);
        } else {
            ++it;
        }
    }

    // Не храним состояния тиков для символов без активности > 5 минут
    for (auto it = symbol_tick_state_.begin(); it != symbol_tick_state_.end();) {
        if ((now_ms - it->second.tick_ms) > 300'000) {
            it = symbol_tick_state_.erase(it);
        } else {
            ++it;
        }
    }

    // Очистка stale baselines
    for (auto it = baselines_.begin(); it != baselines_.end();) {
        if ((now_ms - it->second.last_update_ms) > config_.baseline_stale_reset_ms) {
            it = baselines_.erase(it);
        } else {
            ++it;
        }
    }

    // Очистка stale threat memories
    for (auto it = threat_memories_.begin(); it != threat_memories_.end();) {
        if ((now_ms - it->second.last_update_ms) > 300'000) {
            it = threat_memories_.erase(it);
        } else {
            ++it;
        }
    }

    // Cleanup stale correlations
    for (auto it = correlations_.begin(); it != correlations_.end();) {
        if ((now_ms - it->second.last_update_ms) > 300'000) {
            it = correlations_.erase(it);
        } else {
            ++it;
        }
    }

    // Cleanup stale MTF baselines
    for (auto it = mtf_baselines_.begin(); it != mtf_baselines_.end();) {
        if (it->second.slow.last_update_ms > 0 &&
            (now_ms - it->second.slow.last_update_ms) > config_.baseline_stale_reset_ms) {
            it = mtf_baselines_.erase(it);
        } else {
            ++it;
        }
    }

    // Cleanup stale percentile windows
    // (piggyback on symbol_tick_state cleanup — if tick state is cleaned, clean percentiles too)

    last_cleanup_ms_ = now_ms;
}

void AdversarialMarketDefense::update_symbol_state_locked(const MarketCondition& c) {
    if (c.symbol.get().empty()) return;

    auto& state = symbol_tick_state_[c.symbol.get()];
    state.spread_bps = c.spread_valid ? c.spread_bps : state.spread_bps;
    state.tick_ms = to_milliseconds(c.timestamp);
}

double AdversarialMarketDefense::compute_recovery_multiplier_locked(
    const Symbol& symbol, Timestamp now) const {

    auto it = recovery_until_.find(symbol.get());
    if (it == recovery_until_.end()) {
        return 1.0;
    }

    const int64_t now_ms = to_milliseconds(now);

    // Проверяем: cooldown ещё активен → recovery не начался
    auto cd_it = cooldown_until_.find(symbol.get());
    if (cd_it != cooldown_until_.end() && now_ms < cd_it->second) {
        return 1.0; // cooldown ещё идёт, recovery не применяется
    }

    if (now_ms >= it->second) {
        return 1.0; // recovery период завершён
    }

    // Линейная интерполяция от floor до 1.0
    const int64_t recovery_remaining = it->second - now_ms;
    const double progress = 1.0 - static_cast<double>(recovery_remaining) /
                                  static_cast<double>(config_.recovery_duration_ms);
    return config_.recovery_confidence_floor +
           (1.0 - config_.recovery_confidence_floor) * std::clamp(progress, 0.0, 1.0);
}

double AdversarialMarketDefense::compute_compound_severity(
    const std::vector<ThreatDetection>& threats, double factor) {

    // Вероятностная модель: compound = 1 - ∏(1 - severity_i * factor_weight)
    // Для factor=0 → просто max. Для factor=1 → полный compound.
    if (threats.empty()) return 0.0;

    double max_sev = 0.0;
    double product_complement = 1.0;

    for (const auto& t : threats) {
        if (t.severity <= 0.0) continue;
        max_sev = std::max(max_sev, t.severity);
        product_complement *= (1.0 - t.severity);
    }

    if (max_sev <= 0.0) return 0.0;

    const double probabilistic_severity = 1.0 - product_complement;
    // Blend: (1-factor)*max + factor*probabilistic
    return clamp01((1.0 - factor) * max_sev + factor * probabilistic_severity);
}

// --- Новые детекторы ---

std::optional<ThreatDetection> AdversarialMarketDefense::detect_depth_asymmetry(
    const MarketCondition& c) const {

    if (!c.liquidity_valid) return std::nullopt;
    if (c.bid_depth <= 0.0 || c.ask_depth <= 0.0) return std::nullopt;

    const double max_depth = std::max(c.bid_depth, c.ask_depth);
    const double min_depth = std::min(c.bid_depth, c.ask_depth);
    const double ratio = min_depth / max_depth;

    if (ratio >= config_.depth_asymmetry_threshold) return std::nullopt;

    const double severity = clamp01(
        (config_.depth_asymmetry_threshold - ratio) / config_.depth_asymmetry_threshold);

    return ThreatDetection{
        .type = ThreatType::DepthAsymmetry,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Асимметрия глубины: bid=" + format_double(c.bid_depth) +
                  ", ask=" + format_double(c.ask_depth) +
                  ", ratio=" + format_double(ratio),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_anomalous_baseline(
    const MarketCondition& c) const {

    const auto sym_key = c.symbol.get();
    auto it = baselines_.find(sym_key);
    if (it == baselines_.end()) return std::nullopt;

    const auto& bl = it->second;
    if (!bl.is_warm(config_.baseline_warmup_ticks)) return std::nullopt;

    double max_severity = 0.0;
    std::string anomaly_detail;

    // Z-score спреда: только если статический детектор spread_explosion НЕ сработал
    if (c.spread_valid && c.spread_bps <= config_.spread_explosion_threshold_bps) {
        const double z = bl.z_spread(c.spread_bps);
        if (z > config_.z_score_spread_threshold) {
            double sev = clamp01(
                (z - config_.z_score_spread_threshold) / config_.z_score_spread_threshold);
            if (sev > max_severity) {
                max_severity = sev;
                anomaly_detail = "spread z=" + format_double(z, 2) +
                                 " (ema=" + format_double(bl.spread_ema, 2) + ")";
            }
        }
    }

    // Z-score глубины: только если статический liquidity_vacuum НЕ сработал
    if (c.liquidity_valid) {
        const double depth = std::min(c.bid_depth, c.ask_depth);
        if (depth >= config_.min_liquidity_depth) {
            const double z = bl.z_depth(depth);
            if (z < -config_.z_score_depth_threshold) {
                double sev = clamp01(
                    (-z - config_.z_score_depth_threshold) / config_.z_score_depth_threshold);
                if (sev > max_severity) {
                    max_severity = sev;
                    anomaly_detail = "depth z=" + format_double(z, 2) +
                                     " (ema=" + format_double(bl.depth_ema, 2) + ")";
                }
            }
        }
    }

    // Z-score buy_sell_ratio: только если статический toxic_flow НЕ сработал
    if (c.flow_valid) {
        const double lower_ratio = 1.0 / config_.toxic_flow_ratio_threshold;
        if (c.buy_sell_ratio < config_.toxic_flow_ratio_threshold &&
            c.buy_sell_ratio > lower_ratio) {
            const double z = bl.z_ratio(c.buy_sell_ratio);
            if (std::abs(z) > config_.z_score_ratio_threshold) {
                double sev = clamp01(
                    (std::abs(z) - config_.z_score_ratio_threshold) /
                    config_.z_score_ratio_threshold);
                if (sev > max_severity) {
                    max_severity = sev;
                    anomaly_detail = "ratio z=" + format_double(z, 2) +
                                     " (ema=" + format_double(bl.ratio_ema, 2) + ")";
                }
            }
        }
    }

    if (max_severity <= 0.0) return std::nullopt;

    // Cap severity: anomalous baseline — предупреждающий сигнал, не жёсткий veto
    max_severity = std::min(max_severity, 0.65);

    return ThreatDetection{
        .type = ThreatType::AnomalousBaseline,
        .severity = max_severity,
        .recommended_action = severity_to_action_soft(max_severity),
        .reason = "Z-score аномалия: " + anomaly_detail,
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_threat_escalation(
    const MarketCondition& c, bool has_current_threats) const {

    const auto sym_key = c.symbol.get();
    auto it = threat_memories_.find(sym_key);
    if (it == threat_memories_.end()) return std::nullopt;

    const auto& mem = it->second;
    if (!has_current_threats) return std::nullopt;
    if (mem.consecutive_threats < config_.threat_escalation_ticks) return std::nullopt;

    const int excess = mem.consecutive_threats - config_.threat_escalation_ticks;
    const double severity = clamp01(
        config_.threat_escalation_boost * static_cast<double>(excess + 1));

    if (severity <= 0.0) return std::nullopt;

    return ThreatDetection{
        .type = ThreatType::ThreatEscalation,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Эскалация: " + std::to_string(mem.consecutive_threats) +
                  " consecutive threatening ticks",
        .detected_at = c.timestamp
    };
}

// --- Adaptive Baseline ---

void AdversarialMarketDefense::update_baseline_locked(const MarketCondition& c) {
    if (c.symbol.get().empty()) return;

    const auto sym_key = c.symbol.get();
    const int64_t now_ms = to_milliseconds(c.timestamp);
    auto& bl = baselines_[sym_key];

    // Stale reset
    if (bl.samples > 0 && (now_ms - bl.last_update_ms) > config_.baseline_stale_reset_ms) {
        bl = SymbolBaseline{};
    }

    // Time-weighted alpha: adapts to tick interval
    const double dt_ms = (bl.last_update_ms > 0 && bl.samples > 0)
        ? static_cast<double>(now_ms - bl.last_update_ms) : 0.0;
    const double a = (bl.samples > 0 && dt_ms > 0.0)
        ? time_weighted_alpha(1.0 / config_.baseline_alpha * 50.0, dt_ms)
        : 1.0; // first sample = full init

    if (c.spread_valid && is_finite(c.spread_bps)) {
        if (bl.samples == 0) {
            bl.spread_ema = c.spread_bps;
            bl.spread_ema_sq = c.spread_bps * c.spread_bps;
        } else {
            ema_update(bl.spread_ema, bl.spread_ema_sq, c.spread_bps, a);
        }
    }

    if (c.liquidity_valid) {
        const double depth = std::min(c.bid_depth, c.ask_depth);
        if (is_finite(depth) && depth >= 0.0) {
            if (bl.samples == 0) {
                bl.depth_ema = depth;
                bl.depth_ema_sq = depth * depth;
            } else {
                ema_update(bl.depth_ema, bl.depth_ema_sq, depth, a);
            }
        }
    }

    if (c.flow_valid && is_finite(c.buy_sell_ratio) && c.buy_sell_ratio >= 0.0) {
        if (bl.samples == 0) {
            bl.ratio_ema = c.buy_sell_ratio;
            bl.ratio_ema_sq = c.buy_sell_ratio * c.buy_sell_ratio;
        } else {
            ema_update(bl.ratio_ema, bl.ratio_ema_sq, c.buy_sell_ratio, a);
        }
    }

    bl.samples++;
    bl.last_update_ms = now_ms;
}

// --- Threat Memory ---

void AdversarialMarketDefense::update_threat_memory_locked(
    const std::string& symbol, double compound_severity,
    bool has_threats, int64_t now_ms) {

    if (symbol.empty()) return;

    auto& mem = threat_memories_[symbol];
    const double a = config_.threat_memory_alpha;

    if (has_threats && compound_severity > 0.0) {
        mem.ema_severity = a * compound_severity + (1.0 - a) * mem.ema_severity;
        mem.consecutive_threats++;
        mem.consecutive_safe = 0;
    } else {
        mem.ema_severity = (1.0 - a) * mem.ema_severity; // decay toward 0
        mem.consecutive_safe++;
        mem.consecutive_threats = 0;
    }

    mem.last_update_ms = now_ms;
}

// --- Cross-Signal Amplification ---

double AdversarialMarketDefense::apply_cross_signal_amplification(
    const std::vector<ThreatDetection>& threats, double severity) const {

    if (threats.size() < 2 || severity <= 0.0 || config_.cross_signal_amplification <= 0.0) {
        return severity;
    }

    bool has_spread = false, has_liquidity = false, has_flow = false, has_book = false;
    for (const auto& t : threats) {
        if (t.severity <= 0.0) continue;
        switch (t.type) {
            case ThreatType::SpreadExplosion:
            case ThreatType::SpreadVelocitySpike:
                has_spread = true; break;
            case ThreatType::LiquidityVacuum:
            case ThreatType::DepthAsymmetry:
                has_liquidity = true; break;
            case ThreatType::ToxicFlow:
                has_flow = true; break;
            case ThreatType::UnstableOrderBook:
                has_book = true; break;
            default: break;
        }
    }

    double amplification = 1.0;
    const double amp = config_.cross_signal_amplification;

    // Flash crash pattern: спред расширяется + ликвидность исчезает
    if (has_spread && has_liquidity) amplification += amp * 1.0;
    // Informed trading: токсичный поток + нестабильный стакан
    if (has_flow && has_book) amplification += amp * 0.8;
    // Cascade: спред + поток + ликвидность одновременно
    if (has_spread && has_flow && has_liquidity) amplification += amp * 1.5;
    // Book manipulation: ликвидность + нестабильность стакана
    if (has_liquidity && has_book) amplification += amp * 0.6;

    return clamp01(severity * amplification);
}

// --- Market Regime Classification ---

MarketRegime AdversarialMarketDefense::classify_regime(
    const MarketCondition& c, double compound_severity,
    const std::vector<ThreatDetection>& threats) const {

    // Простой heuristic классификатор
    bool has_toxic_flow = false;
    bool has_liquidity_issue = false;
    bool has_spread_issue = false;

    for (const auto& t : threats) {
        if (t.severity <= 0.0) continue;
        switch (t.type) {
            case ThreatType::ToxicFlow:
                has_toxic_flow = true; break;
            case ThreatType::LiquidityVacuum:
            case ThreatType::DepthAsymmetry:
                has_liquidity_issue = true; break;
            case ThreatType::SpreadExplosion:
            case ThreatType::SpreadVelocitySpike:
                has_spread_issue = true; break;
            default: break;
        }
    }

    // Toxic: compound > 0.7 ИЛИ вето-уровень ИЛИ комбинация flow + другие
    if (compound_severity > 0.7 || (has_toxic_flow && (has_spread_issue || has_liquidity_issue))) {
        return MarketRegime::Toxic;
    }

    // Low liquidity: liquidity issue как основная проблема
    if (has_liquidity_issue && !has_spread_issue) {
        return MarketRegime::LowLiquidity;
    }

    // Volatile: spread issues или compound средний
    if (has_spread_issue || compound_severity > 0.3) {
        return MarketRegime::Volatile;
    }

    // Low liquidity: даже с другими mild issues
    if (has_liquidity_issue) {
        return MarketRegime::LowLiquidity;
    }

    // Normal: compound низкий, нет значимых угроз
    if (compound_severity < 0.1) {
        return MarketRegime::Normal;
    }

    return MarketRegime::Unknown;
}

// --- Diagnostics API ---

DefenseDiagnostics AdversarialMarketDefense::get_diagnostics(
    const Symbol& symbol, Timestamp now) const {

    std::lock_guard<std::mutex> lock(mutex_);

    DefenseDiagnostics diag;
    diag.symbol = symbol.get();

    const int64_t now_ms = to_milliseconds(now);

    // Baseline
    auto bl_it = baselines_.find(symbol.get());
    if (bl_it != baselines_.end()) {
        const auto& bl = bl_it->second;
        diag.baseline_warm = bl.is_warm(config_.baseline_warmup_ticks);
        diag.baseline_samples = bl.samples;
        diag.spread_ema = bl.spread_ema;
        diag.depth_ema = bl.depth_ema;
        diag.ratio_ema = bl.ratio_ema;
    }

    // Threat memory
    auto mem_it = threat_memories_.find(symbol.get());
    if (mem_it != threat_memories_.end()) {
        const auto& mem = mem_it->second;
        diag.threat_memory_severity = mem.ema_severity;
        diag.consecutive_threats = mem.consecutive_threats;
        diag.consecutive_safe = mem.consecutive_safe;
    }

    // Cooldown
    auto cd_it = cooldown_until_.find(symbol.get());
    if (cd_it != cooldown_until_.end() && now_ms < cd_it->second) {
        diag.cooldown_active = true;
        diag.cooldown_remaining_ms = cd_it->second - now_ms;
    }

    // Recovery
    auto rc_it = recovery_until_.find(symbol.get());
    if (rc_it != recovery_until_.end() && now_ms < rc_it->second) {
        if (!diag.cooldown_active) {
            diag.in_recovery = true;
        }
    }

    // v4: Hysteresis
    auto hys_it = hysteresis_active_.find(symbol.get());
    if (hys_it != hysteresis_active_.end()) {
        diag.hysteresis_active = hys_it->second;
    }

    // v4: Correlations
    auto corr_it = correlations_.find(symbol.get());
    if (corr_it != correlations_.end()) {
        diag.spread_depth_correlation = corr_it->second.corr_spread_depth();
        diag.spread_flow_correlation = corr_it->second.corr_spread_flow();
        diag.depth_flow_correlation = corr_it->second.corr_depth_flow();
    }

    // v4: Multi-timeframe
    auto mtf_it = mtf_baselines_.find(symbol.get());
    if (mtf_it != mtf_baselines_.end()) {
        diag.fast_spread_ema = mtf_it->second.fast.spread_ema;
        diag.slow_spread_ema = mtf_it->second.slow.spread_ema;
    }

    // v4: Percentile
    auto pct_it = percentile_windows_.find(symbol.get());
    if (pct_it != percentile_windows_.end() && !pct_it->second.spread_history.empty()) {
        // Compute current percentile for diagnostics
        diag.spread_percentile = compute_percentile(
            pct_it->second.spread_history, diag.spread_ema);
    }

    // v4: Calibration
    diag.calibration = calibration_;

    return diag;
}

// --- v4: Static helpers ---

double AdversarialMarketDefense::time_weighted_alpha(double halflife_ms, double dt_ms) {
    if (dt_ms <= 0.0 || halflife_ms <= 0.0) return 0.0;
    // alpha = 1 - exp(-dt * ln2 / halflife)
    return 1.0 - std::exp(-dt_ms * 0.693147180559945 / halflife_ms);
}

void AdversarialMarketDefense::ema_update(double& ema, double& ema_sq,
                                           double value, double alpha) {
    ema = alpha * value + (1.0 - alpha) * ema;
    ema_sq = alpha * value * value + (1.0 - alpha) * ema_sq;
}

double AdversarialMarketDefense::compute_percentile(const std::deque<double>& window,
                                                     double value) {
    if (window.empty()) return 0.5;
    int count_below = 0;
    int count_equal = 0;
    for (const double v : window) {
        if (v < value) ++count_below;
        else if (std::abs(v - value) < 1e-12) ++count_equal;
    }
    // Midpoint percentile (Hazen): handles ties correctly
    return (static_cast<double>(count_below) + 0.5 * static_cast<double>(count_equal))
           / static_cast<double>(window.size());
}

// --- v4: Percentile scoring ---

void AdversarialMarketDefense::update_percentile_window_locked(const MarketCondition& c) {
    if (c.symbol.get().empty()) return;
    auto& pw = percentile_windows_[c.symbol.get()];
    const auto max_size = static_cast<size_t>(config_.percentile_window_size);

    if (c.spread_valid && is_finite(c.spread_bps)) {
        pw.spread_history.push_back(c.spread_bps);
        if (pw.spread_history.size() > max_size) pw.spread_history.pop_front();
    }
    if (c.liquidity_valid) {
        double depth = std::min(c.bid_depth, c.ask_depth);
        if (is_finite(depth) && depth >= 0.0) {
            pw.depth_history.push_back(depth);
            if (pw.depth_history.size() > max_size) pw.depth_history.pop_front();
        }
    }
    if (c.flow_valid && is_finite(c.buy_sell_ratio) && c.buy_sell_ratio >= 0.0) {
        pw.ratio_history.push_back(c.buy_sell_ratio);
        if (pw.ratio_history.size() > max_size) pw.ratio_history.pop_front();
    }
}

double AdversarialMarketDefense::compute_percentile_severity_locked(
    const std::string& symbol, const MarketCondition& c) const {

    auto it = percentile_windows_.find(symbol);
    if (it == percentile_windows_.end()) return 0.0;

    const auto& pw = it->second;
    const auto min_window = static_cast<size_t>(config_.percentile_window_size / 4);
    double max_severity = 0.0;

    // Spread: higher percentile = worse
    if (c.spread_valid && pw.spread_history.size() >= min_window) {
        double pct = compute_percentile(pw.spread_history, c.spread_bps);
        if (pct > config_.percentile_severity_threshold) {
            double sev = clamp01((pct - config_.percentile_severity_threshold) /
                                 (1.0 - config_.percentile_severity_threshold));
            max_severity = std::max(max_severity, sev);
        }
    }

    // Depth: lower percentile = worse (inverted)
    if (c.liquidity_valid && pw.depth_history.size() >= min_window) {
        double depth = std::min(c.bid_depth, c.ask_depth);
        double pct = compute_percentile(pw.depth_history, depth);
        double inv_pct = 1.0 - pct; // invert: low depth = high percentile danger
        if (inv_pct > config_.percentile_severity_threshold) {
            double sev = clamp01((inv_pct - config_.percentile_severity_threshold) /
                                 (1.0 - config_.percentile_severity_threshold));
            max_severity = std::max(max_severity, sev);
        }
    }

    // Ratio: extreme percentile either way = worse
    if (c.flow_valid && pw.ratio_history.size() >= min_window) {
        double pct = compute_percentile(pw.ratio_history, c.buy_sell_ratio);
        double extreme = std::max(pct, 1.0 - pct); // distance from median
        if (extreme > config_.percentile_severity_threshold) {
            double sev = clamp01((extreme - config_.percentile_severity_threshold) /
                                 (1.0 - config_.percentile_severity_threshold));
            max_severity = std::max(max_severity, sev * 0.7); // ratio gets less weight
        }
    }

    return max_severity;
}

// --- v4: Correlation matrix ---

double AdversarialMarketDefense::CorrelationState::corr_spread_depth() const {
    double denom = std::sqrt(spread_var * depth_var);
    return denom < 1e-12 ? 0.0 : spread_depth_cov / denom;
}
double AdversarialMarketDefense::CorrelationState::corr_spread_flow() const {
    double denom = std::sqrt(spread_var * flow_var);
    return denom < 1e-12 ? 0.0 : spread_flow_cov / denom;
}
double AdversarialMarketDefense::CorrelationState::corr_depth_flow() const {
    double denom = std::sqrt(depth_var * flow_var);
    return denom < 1e-12 ? 0.0 : depth_flow_cov / denom;
}

void AdversarialMarketDefense::update_correlation_locked(const MarketCondition& c) {
    if (c.symbol.get().empty() || !c.spread_valid || !c.liquidity_valid || !c.flow_valid)
        return;

    const double spread = c.spread_bps;
    const double depth = std::min(c.bid_depth, c.ask_depth);
    const double flow = c.buy_sell_ratio;
    if (!is_finite(spread) || !is_finite(depth) || !is_finite(flow)) return;

    auto& cs = correlations_[c.symbol.get()];
    const int64_t now_ms = to_milliseconds(c.timestamp);
    const double dt_ms = (cs.last_update_ms > 0) ? static_cast<double>(now_ms - cs.last_update_ms) : 0.0;
    const double a = (dt_ms > 0.0 && cs.samples > 0)
        ? time_weighted_alpha(1.0 / config_.correlation_alpha * 50.0, dt_ms)
        : 1.0;

    if (cs.samples == 0) {
        cs.spread_mean = spread;
        cs.depth_mean = depth;
        cs.flow_mean = flow;
        cs.spread_var = 0.0;
        cs.depth_var = 0.0;
        cs.flow_var = 0.0;
        cs.spread_depth_cov = 0.0;
        cs.spread_flow_cov = 0.0;
        cs.depth_flow_cov = 0.0;
    } else {
        double ds = spread - cs.spread_mean;
        double dd = depth - cs.depth_mean;
        double df = flow - cs.flow_mean;

        // Save previous correlations for delta detection
        cs.prev_spread_depth_corr = cs.corr_spread_depth();
        cs.prev_spread_flow_corr = cs.corr_spread_flow();
        cs.prev_depth_flow_corr = cs.corr_depth_flow();

        cs.spread_mean = a * spread + (1.0 - a) * cs.spread_mean;
        cs.depth_mean = a * depth + (1.0 - a) * cs.depth_mean;
        cs.flow_mean = a * flow + (1.0 - a) * cs.flow_mean;

        cs.spread_var = a * ds * ds + (1.0 - a) * cs.spread_var;
        cs.depth_var = a * dd * dd + (1.0 - a) * cs.depth_var;
        cs.flow_var = a * df * df + (1.0 - a) * cs.flow_var;
        cs.spread_depth_cov = a * ds * dd + (1.0 - a) * cs.spread_depth_cov;
        cs.spread_flow_cov = a * ds * df + (1.0 - a) * cs.spread_flow_cov;
        cs.depth_flow_cov = a * dd * df + (1.0 - a) * cs.depth_flow_cov;
    }

    cs.samples++;
    cs.last_update_ms = now_ms;
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_correlation_breakdown(
    const MarketCondition& c) const {

    auto it = correlations_.find(c.symbol.get());
    if (it == correlations_.end() || it->second.samples < 50) return std::nullopt;

    const auto& cs = it->second;
    const double threshold = config_.correlation_breakdown_threshold;

    // Check for sudden correlation changes
    double max_delta = 0.0;
    std::string detail;

    double d1 = std::abs(cs.corr_spread_depth() - cs.prev_spread_depth_corr);
    double d2 = std::abs(cs.corr_spread_flow() - cs.prev_spread_flow_corr);
    double d3 = std::abs(cs.corr_depth_flow() - cs.prev_depth_flow_corr);

    if (d1 > max_delta) { max_delta = d1; detail = "spread-depth Δ=" + format_double(d1, 3); }
    if (d2 > max_delta) { max_delta = d2; detail = "spread-flow Δ=" + format_double(d2, 3); }
    if (d3 > max_delta) { max_delta = d3; detail = "depth-flow Δ=" + format_double(d3, 3); }

    if (max_delta < threshold) return std::nullopt;

    double severity = clamp01((max_delta - threshold) / threshold);
    severity = std::min(severity, 0.7); // cap: correlation is a warning, not hard veto

    return ThreatDetection{
        .type = ThreatType::CorrelationBreakdown,
        .severity = severity,
        .recommended_action = severity_to_action_soft(severity),
        .reason = "Распад корреляции: " + detail,
        .detected_at = c.timestamp
    };
}

// --- v4: Multi-timeframe baselines ---

void AdversarialMarketDefense::update_mtf_baselines_locked(const MarketCondition& c) {
    if (c.symbol.get().empty()) return;

    const int64_t now_ms = to_milliseconds(c.timestamp);
    auto& mtf = mtf_baselines_[c.symbol.get()];

    auto update_single = [&](SymbolBaseline& bl, double halflife_ms) {
        const double dt_ms = (bl.last_update_ms > 0)
            ? static_cast<double>(now_ms - bl.last_update_ms) : 0.0;
        const double a = (bl.samples > 0 && dt_ms > 0.0)
            ? time_weighted_alpha(halflife_ms, dt_ms)
            : 1.0;

        if (c.spread_valid && is_finite(c.spread_bps)) {
            if (bl.samples == 0) {
                bl.spread_ema = c.spread_bps;
                bl.spread_ema_sq = c.spread_bps * c.spread_bps;
            } else {
                ema_update(bl.spread_ema, bl.spread_ema_sq, c.spread_bps, a);
            }
        }
        if (c.liquidity_valid) {
            double depth = std::min(c.bid_depth, c.ask_depth);
            if (is_finite(depth) && depth >= 0.0) {
                if (bl.samples == 0) {
                    bl.depth_ema = depth;
                    bl.depth_ema_sq = depth * depth;
                } else {
                    ema_update(bl.depth_ema, bl.depth_ema_sq, depth, a);
                }
            }
        }
        bl.samples++;
        bl.last_update_ms = now_ms;
    };

    update_single(mtf.fast, config_.baseline_halflife_fast_ms);
    update_single(mtf.medium, config_.baseline_halflife_medium_ms);
    update_single(mtf.slow, config_.baseline_halflife_slow_ms);
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_timeframe_divergence(
    const MarketCondition& c) const {

    auto it = mtf_baselines_.find(c.symbol.get());
    if (it == mtf_baselines_.end()) return std::nullopt;

    const auto& mtf = it->second;
    // Need warm fast AND slow baselines
    if (mtf.fast.samples < 30 || mtf.slow.samples < 100) return std::nullopt;

    double max_severity = 0.0;
    std::string detail;

    // Compare fast vs slow spread EMAs using slow's std as reference
    {
        double slow_std = mtf.slow.spread_std();
        double ref_std = std::max(slow_std, std::max(1.0, mtf.slow.spread_ema * 0.02));
        double z = (mtf.fast.spread_ema - mtf.slow.spread_ema) / ref_std;
        if (std::abs(z) > config_.timeframe_divergence_threshold) {
            double sev = clamp01(
                (std::abs(z) - config_.timeframe_divergence_threshold) /
                config_.timeframe_divergence_threshold);
            if (sev > max_severity) {
                max_severity = sev;
                detail = "spread fast/slow z=" + format_double(z, 2) +
                         " (fast=" + format_double(mtf.fast.spread_ema, 2) +
                         ", slow=" + format_double(mtf.slow.spread_ema, 2) + ")";
            }
        }
    }

    // Compare fast vs slow depth EMAs
    {
        double slow_std = mtf.slow.depth_std();
        double ref_std = std::max(slow_std, std::max(1.0, mtf.slow.depth_ema * 0.02));
        double z = (mtf.fast.depth_ema - mtf.slow.depth_ema) / ref_std;
        if (std::abs(z) > config_.timeframe_divergence_threshold) {
            double sev = clamp01(
                (std::abs(z) - config_.timeframe_divergence_threshold) /
                config_.timeframe_divergence_threshold);
            if (sev > max_severity) {
                max_severity = sev;
                detail = "depth fast/slow z=" + format_double(z, 2) +
                         " (fast=" + format_double(mtf.fast.depth_ema, 2) +
                         ", slow=" + format_double(mtf.slow.depth_ema, 2) + ")";
            }
        }
    }

    if (max_severity <= 0.0) return std::nullopt;

    max_severity = std::min(max_severity, 0.6); // divergence is a warning signal

    return ThreatDetection{
        .type = ThreatType::TimeframeDivergence,
        .severity = max_severity,
        .recommended_action = severity_to_action_soft(max_severity),
        .reason = "Multi-TF расхождение: " + detail,
        .detected_at = c.timestamp
    };
}

// --- v4: Hysteresis ---

bool AdversarialMarketDefense::update_hysteresis_locked(
    const std::string& symbol, double compound_severity) {

    auto& active = hysteresis_active_[symbol];

    if (active) {
        // Currently in danger zone — exit only below lower threshold
        if (compound_severity < config_.hysteresis_exit_severity) {
            active = false;
            calibration_.hysteresis_deactivations++;
        }
    } else {
        // Currently safe — enter only above upper threshold
        if (compound_severity > config_.hysteresis_enter_severity) {
            active = true;
            calibration_.hysteresis_activations++;
        }
    }

    return active;
}

// --- v4: Event sourcing ---

void AdversarialMarketDefense::emit_event_locked(const DefenseAssessment& result, int64_t now_ms) {
    if (config_.audit_log_max_size <= 0) return;

    DefenseEvent event;
    event.timestamp_ms = now_ms;
    event.symbol = result.symbol.get();
    event.action = result.overall_action;
    event.compound_severity = result.compound_severity;
    event.confidence_multiplier = result.confidence_multiplier;
    event.threshold_multiplier = result.threshold_multiplier;
    event.regime = result.regime;
    event.threat_count = static_cast<int>(result.threats.size());
    event.is_safe = result.is_safe;
    event.hysteresis_active = result.hysteresis_active;

    audit_log_.push_back(std::move(event));
    while (static_cast<int64_t>(audit_log_.size()) > config_.audit_log_max_size) {
        audit_log_.pop_front();
    }
}

std::vector<DefenseEvent> AdversarialMarketDefense::get_audit_log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {audit_log_.begin(), audit_log_.end()};
}

// --- v4: Calibration metrics ---

void AdversarialMarketDefense::update_calibration_locked(
    const DefenseAssessment& result,
    const std::vector<ThreatDetection>& threats) {

    calibration_.total_assessments++;
    if (result.is_safe) calibration_.safe_count++;

    switch (result.overall_action) {
        case DefenseAction::VetoTrade:        calibration_.veto_count++; break;
        case DefenseAction::Cooldown:         calibration_.cooldown_count++; break;
        case DefenseAction::RaiseThreshold:   calibration_.raise_threshold_count++; break;
        case DefenseAction::ReduceConfidence: calibration_.reduce_confidence_count++; break;
        default: break;
    }

    // Running average of compound severity
    double n = static_cast<double>(calibration_.total_assessments);
    calibration_.avg_compound_severity =
        calibration_.avg_compound_severity * ((n - 1.0) / n) +
        result.compound_severity / n;
    calibration_.max_compound_severity =
        std::max(calibration_.max_compound_severity, result.compound_severity);
    calibration_.veto_rate = static_cast<double>(calibration_.veto_count) / n;

    // Per-threat type counts
    for (const auto& t : threats) {
        if (t.severity <= 0.0) continue;
        switch (t.type) {
            case ThreatType::SpreadExplosion:      calibration_.spread_explosion_count++; break;
            case ThreatType::LiquidityVacuum:      calibration_.liquidity_vacuum_count++; break;
            case ThreatType::ToxicFlow:            calibration_.toxic_flow_count++; break;
            case ThreatType::DepthAsymmetry:       calibration_.depth_asymmetry_count++; break;
            case ThreatType::CorrelationBreakdown: calibration_.correlation_breakdown_count++; break;
            case ThreatType::TimeframeDivergence:  calibration_.timeframe_divergence_count++; break;
            case ThreatType::AnomalousBaseline:    calibration_.anomalous_baseline_count++; break;
            case ThreatType::ThreatEscalation:     calibration_.escalation_count++; break;
            default: break;
        }
    }
}

CalibrationMetrics AdversarialMarketDefense::get_calibration_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calibration_;
}

void AdversarialMarketDefense::reset_calibration_metrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    calibration_ = CalibrationMetrics{};
}

} // namespace tb::adversarial
