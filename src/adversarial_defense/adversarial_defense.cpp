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
    }

    // Вычислить compound severity вероятностной моделью
    const double compound_severity = compute_compound_severity(
        result.threats, config_.compound_threat_factor);
    result.compound_severity = compound_severity;

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

} // namespace tb::adversarial
