/**
 * @file adversarial_defense.cpp
 * @brief Реализация защиты от враждебных рыночных условий
 */

#include "adversarial_defense.hpp"

#include <algorithm>
#include <cmath>

namespace tb::adversarial {

AdversarialMarketDefense::AdversarialMarketDefense(DefenseConfig config)
    : config_(std::move(config)) {}

DefenseAssessment AdversarialMarketDefense::assess(const MarketCondition& condition) {
    std::lock_guard<std::mutex> lock(mutex_);

    DefenseAssessment result;
    result.symbol = condition.symbol;
    result.assessed_at = condition.timestamp;

    // 1. Проверяем cooldown — если активен, торговля запрещена
    auto cooldown_threat = check_cooldown(condition.symbol, condition.timestamp);
    if (cooldown_threat.recommended_action != DefenseAction::NoAction) {
        result.threats.push_back(cooldown_threat);
        result.cooldown_active = true;

        auto it = cooldown_until_.find(condition.symbol.get());
        if (it != cooldown_until_.end()) {
            int64_t now_ms = condition.timestamp.get() / 1'000'000; // нано → мс
            result.cooldown_remaining_ms = std::max(int64_t{0}, it->second - now_ms);
        }
    }

    // 2. Проверяем взрыв спреда
    if (auto threat = detect_spread_explosion(condition)) {
        result.threats.push_back(*threat);
    }

    // 3. Проверяем вакуум ликвидности
    if (auto threat = detect_liquidity_vacuum(condition)) {
        result.threats.push_back(*threat);
    }

    // 4. Проверяем нестабильность стакана
    if (auto threat = detect_unstable_book(condition)) {
        result.threats.push_back(*threat);
    }

    // 5. Проверяем токсичный поток
    if (auto threat = detect_toxic_flow(condition)) {
        result.threats.push_back(*threat);
    }

    // 6. Проверяем ловушку ложного пробоя
    if (auto threat = detect_bad_breakout(condition)) {
        result.threats.push_back(*threat);
    }

    // Комбинирование: выбираем наиболее серьёзное действие
    double max_severity = 0.0;
    for (const auto& t : result.threats) {
        max_severity = std::max(max_severity, t.severity);

        // VetoTrade — приоритетное действие
        if (t.recommended_action == DefenseAction::VetoTrade) {
            result.overall_action = DefenseAction::VetoTrade;
        } else if (result.overall_action != DefenseAction::VetoTrade) {
            // Выбираем более строгое действие
            if (static_cast<int>(t.recommended_action) > static_cast<int>(result.overall_action)) {
                result.overall_action = t.recommended_action;
            }
        }
    }

    // Безопасность: VetoTrade или Cooldown означают небезопасно
    result.is_safe = (result.overall_action != DefenseAction::VetoTrade &&
                      result.overall_action != DefenseAction::Cooldown);

    // Множитель уверенности: снижается с увеличением серьёзности
    result.confidence_multiplier = std::max(0.0, 1.0 - max_severity * 0.8);

    // Множитель порога: растёт с увеличением серьёзности
    result.threshold_multiplier = 1.0 + max_severity * 2.0;

    return result;
}

void AdversarialMarketDefense::register_shock(Symbol symbol, ThreatType type, Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t now_ms = now.get() / 1'000'000; // нано → мс
    int64_t duration = (type == ThreatType::PostShockCooldown)
        ? config_.post_shock_cooldown_ms
        : config_.cooldown_duration_ms;

    cooldown_until_[symbol.get()] = now_ms + duration;
}

bool AdversarialMarketDefense::is_cooldown_active(const Symbol& symbol, Timestamp now) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cooldown_until_.find(symbol.get());
    if (it == cooldown_until_.end()) return false;

    int64_t now_ms = now.get() / 1'000'000;
    return now_ms < it->second;
}

void AdversarialMarketDefense::reset_cooldown(const Symbol& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    cooldown_until_.erase(symbol.get());
}

// --- Детекторы угроз ---

std::optional<ThreatDetection> AdversarialMarketDefense::detect_spread_explosion(
    const MarketCondition& c) const {

    if (c.spread_bps <= config_.spread_explosion_threshold_bps) return std::nullopt;

    double severity = std::min(1.0, c.spread_bps / (2.0 * config_.spread_explosion_threshold_bps));

    return ThreatDetection{
        .type = ThreatType::SpreadExplosion,
        .severity = severity,
        .recommended_action = DefenseAction::VetoTrade,
        .reason = "Спред " + std::to_string(c.spread_bps) +
                  " bps превышает порог " + std::to_string(config_.spread_explosion_threshold_bps),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_liquidity_vacuum(
    const MarketCondition& c) const {

    double min_depth = std::min(c.bid_depth, c.ask_depth);
    if (min_depth >= config_.min_liquidity_depth) return std::nullopt;

    double ratio = min_depth / config_.min_liquidity_depth;
    double severity = std::min(1.0, 1.0 - ratio);

    return ThreatDetection{
        .type = ThreatType::LiquidityVacuum,
        .severity = severity,
        .recommended_action = (severity > 0.7) ? DefenseAction::VetoTrade : DefenseAction::ReduceConfidence,
        .reason = "Минимальная глубина " + std::to_string(min_depth) +
                  " ниже порога " + std::to_string(config_.min_liquidity_depth),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_unstable_book(
    const MarketCondition& c) const {

    // Невалидный стакан — критическая угроза
    if (!c.book_valid) {
        return ThreatDetection{
            .type = ThreatType::UnstableOrderBook,
            .severity = 1.0,
            .recommended_action = DefenseAction::VetoTrade,
            .reason = "Стакан невалиден",
            .detected_at = c.timestamp
        };
    }

    if (c.book_instability <= config_.book_instability_threshold) return std::nullopt;

    double severity = std::min(1.0,
        (c.book_instability - config_.book_instability_threshold) /
        (1.0 - config_.book_instability_threshold));

    return ThreatDetection{
        .type = ThreatType::UnstableOrderBook,
        .severity = severity,
        .recommended_action = DefenseAction::ReduceConfidence,
        .reason = "Нестабильность стакана " + std::to_string(c.book_instability) +
                  " превышает порог " + std::to_string(config_.book_instability_threshold),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_toxic_flow(
    const MarketCondition& c) const {

    // Токсичный поток: отношение покупок к продажам слишком высокое или низкое
    bool toxic = (c.buy_sell_ratio > config_.toxic_flow_threshold) ||
                 (c.buy_sell_ratio < (1.0 - config_.toxic_flow_threshold));

    if (!toxic) return std::nullopt;

    double deviation = std::max(
        c.buy_sell_ratio - config_.toxic_flow_threshold,
        (1.0 - config_.toxic_flow_threshold) - c.buy_sell_ratio
    );
    double severity = std::min(1.0, deviation / 0.15);

    return ThreatDetection{
        .type = ThreatType::ToxicFlow,
        .severity = severity,
        .recommended_action = DefenseAction::RaiseThreshold,
        .reason = "Токсичный поток: buy/sell ratio = " + std::to_string(c.buy_sell_ratio),
        .detected_at = c.timestamp
    };
}

std::optional<ThreatDetection> AdversarialMarketDefense::detect_bad_breakout(
    const MarketCondition& c) const {

    // Ловушка ложного пробоя: расширенный спред + сильный дисбаланс
    bool spread_expanded = c.spread_bps > config_.spread_normal_bps * 2.0;
    bool strong_imbalance = c.book_imbalance > config_.book_imbalance_threshold;

    if (!spread_expanded || !strong_imbalance) return std::nullopt;

    double severity = std::min(1.0,
        (c.spread_bps / (config_.spread_normal_bps * 4.0)) *
        (c.book_imbalance / 1.0));

    return ThreatDetection{
        .type = ThreatType::BadBreakoutTrap,
        .severity = severity * 0.6, // Ловушки менее серьёзны
        .recommended_action = DefenseAction::ReduceConfidence,
        .reason = "Возможная ловушка: спред " + std::to_string(c.spread_bps) +
                  " bps + дисбаланс " + std::to_string(c.book_imbalance),
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

    int64_t now_ms = now.get() / 1'000'000;
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
        .recommended_action = DefenseAction::VetoTrade,
        .reason = "Cooldown активен для " + symbol.get(),
        .detected_at = now
    };
}

} // namespace tb::adversarial
