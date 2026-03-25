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

    // Threat memory: residual confidence reduction при отсутствии текущих угроз
    // (reads memory from PREVIOUS ticks — updated below)
    const auto sym_key = condition.symbol.get();
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
        .recommended_action = severity_to_action(severity),
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
        .recommended_action = severity_to_action(severity),
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
        .recommended_action = severity_to_action(severity),
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
        .recommended_action = severity_to_action(severity),
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
        .recommended_action = severity_to_action(max_severity),
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
        .recommended_action = severity_to_action(severity),
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
    const double a = config_.baseline_alpha;

    // Сброс при stale baseline
    if (bl.samples > 0 && (now_ms - bl.last_update_ms) > config_.baseline_stale_reset_ms) {
        bl = SymbolBaseline{};
    }

    // Спред
    if (c.spread_valid && is_finite(c.spread_bps)) {
        if (bl.samples == 0) {
            bl.spread_ema = c.spread_bps;
            bl.spread_ema_sq = c.spread_bps * c.spread_bps;
        } else {
            bl.spread_ema = a * c.spread_bps + (1.0 - a) * bl.spread_ema;
            bl.spread_ema_sq = a * c.spread_bps * c.spread_bps +
                               (1.0 - a) * bl.spread_ema_sq;
        }
    }

    // Глубина (min bid/ask)
    if (c.liquidity_valid) {
        const double depth = std::min(c.bid_depth, c.ask_depth);
        if (is_finite(depth) && depth >= 0.0) {
            if (bl.samples == 0) {
                bl.depth_ema = depth;
                bl.depth_ema_sq = depth * depth;
            } else {
                bl.depth_ema = a * depth + (1.0 - a) * bl.depth_ema;
                bl.depth_ema_sq = a * depth * depth + (1.0 - a) * bl.depth_ema_sq;
            }
        }
    }

    // Buy/sell ratio
    if (c.flow_valid && is_finite(c.buy_sell_ratio) && c.buy_sell_ratio >= 0.0) {
        if (bl.samples == 0) {
            bl.ratio_ema = c.buy_sell_ratio;
            bl.ratio_ema_sq = c.buy_sell_ratio * c.buy_sell_ratio;
        } else {
            bl.ratio_ema = a * c.buy_sell_ratio + (1.0 - a) * bl.ratio_ema;
            bl.ratio_ema_sq = a * c.buy_sell_ratio * c.buy_sell_ratio +
                              (1.0 - a) * bl.ratio_ema_sq;
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

    return diag;
}

} // namespace tb::adversarial
