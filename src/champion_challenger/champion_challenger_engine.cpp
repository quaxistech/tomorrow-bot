/**
 * @file champion_challenger_engine.cpp
 * @brief Реализация движка Champion-Challenger A/B тестирования
 *
 * Позволяет сравнивать производительность новых стратегий (challenger)
 * с действующей конфигурацией (champion) и принимать решения
 * о повышении или отклонении.
 */
#include "champion_challenger/champion_challenger_engine.hpp"
#include <cmath>

namespace tb::champion_challenger {

ChampionChallengerEngine::ChampionChallengerEngine(ChampionChallengerConfig config)
    : config_(std::move(config)) {}

VoidResult ChampionChallengerEngine::register_challenger(
    StrategyId champion,
    StrategyId challenger,
    StrategyVersion version)
{
    std::lock_guard lock(mutex_);

    const auto& key = challenger.get();
    if (entries_.contains(key)) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    ChallengerEntry entry;
    entry.challenger_id = challenger;
    entry.champion_id = champion;
    entry.version = version;
    entry.status = ChallengerStatus::Registered;

    entries_[key] = std::move(entry);
    return OkVoid();
}

VoidResult ChampionChallengerEngine::record_champion_outcome(
    StrategyId champion,
    double pnl_bps,
    const std::string& regime)
{
    std::lock_guard lock(mutex_);

    // Обновляем метрики champion для всех связанных challenger'ов
    for (auto& [key, entry] : entries_) {
        if (entry.champion_id.get() != champion.get()) continue;
        if (entry.status == ChallengerStatus::Promoted ||
            entry.status == ChallengerStatus::Rejected ||
            entry.status == ChallengerStatus::Retired) continue;

        // Переводим в статус оценки при первой записи
        if (entry.status == ChallengerStatus::Registered) {
            entry.status = ChallengerStatus::Evaluating;
        }

        auto& m = entry.champion_metrics;
        m.hypothetical_pnl_bps += pnl_bps;
        m.decision_count += 1;
        m.regime_pnl[regime] += pnl_bps;
        if (pnl_bps > 0.0) {
            m.profitable_count += 1;
        }
    }

    return OkVoid();
}

VoidResult ChampionChallengerEngine::record_challenger_outcome(
    StrategyId challenger,
    double pnl_bps,
    const std::string& regime,
    double conviction)
{
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto& entry = it->second;
    if (entry.status == ChallengerStatus::Promoted ||
        entry.status == ChallengerStatus::Rejected ||
        entry.status == ChallengerStatus::Retired) {
        return OkVoid();
    }

    // Переводим в статус оценки при первой записи
    if (entry.status == ChallengerStatus::Registered) {
        entry.status = ChallengerStatus::Evaluating;
    }

    auto& m = entry.challenger_metrics;
    m.hypothetical_pnl_bps += pnl_bps;
    m.decision_count += 1;
    m.regime_pnl[regime] += pnl_bps;
    if (pnl_bps > 0.0) {
        m.profitable_count += 1;
    }

    // Пересчитываем среднюю уверенность инкрементально
    m.avg_conviction =
        ((m.avg_conviction * static_cast<double>(m.decision_count - 1)) + conviction)
        / static_cast<double>(m.decision_count);

    return OkVoid();
}

Result<ChampionChallengerReport> ChampionChallengerEngine::evaluate(StrategyId champion) const {
    std::lock_guard lock(mutex_);

    ChampionChallengerReport report;
    report.champion_id = champion;

    for (const auto& [key, entry] : entries_) {
        if (entry.champion_id.get() != champion.get()) continue;
        report.challengers.push_back(entry);
    }

    return Ok(std::move(report));
}

bool ChampionChallengerEngine::should_promote(StrategyId challenger) const {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) return false;

    const auto& entry = it->second;
    if (entry.status != ChallengerStatus::Evaluating) return false;

    // Недостаточно данных для оценки
    if (entry.challenger_metrics.decision_count < config_.min_evaluation_trades) return false;

    double delta = compute_performance_delta(entry);
    return delta >= config_.promotion_threshold;
}

bool ChampionChallengerEngine::should_reject(StrategyId challenger) const {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) return false;

    const auto& entry = it->second;
    if (entry.status != ChallengerStatus::Evaluating) return false;

    // Недостаточно данных для оценки
    if (entry.challenger_metrics.decision_count < config_.min_evaluation_trades) return false;

    double delta = compute_performance_delta(entry);
    return delta <= config_.rejection_threshold;
}

VoidResult ChampionChallengerEngine::promote(StrategyId challenger) {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    it->second.status = ChallengerStatus::Promoted;
    it->second.promotion_reason =
        "Challenger превзошёл champion по P&L на " +
        std::to_string(compute_performance_delta(it->second) * 100.0) + "%";

    return OkVoid();
}

VoidResult ChampionChallengerEngine::reject(StrategyId challenger) {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    it->second.status = ChallengerStatus::Rejected;
    it->second.rejection_reason =
        "Challenger отстаёт от champion по P&L на " +
        std::to_string(std::abs(compute_performance_delta(it->second)) * 100.0) + "%";

    return OkVoid();
}

double ChampionChallengerEngine::compute_performance_delta(const ChallengerEntry& entry) const {
    // Относительная разница P&L: (challenger - champion) / |champion|
    // Если champion P&L = 0, используем абсолютную разницу нормированную
    const double champ_pnl = entry.champion_metrics.hypothetical_pnl_bps;
    const double chall_pnl = entry.challenger_metrics.hypothetical_pnl_bps;

    if (std::abs(champ_pnl) < 1e-9) {
        // Champion на нуле — если challenger положительный, это улучшение
        return chall_pnl > 0.0 ? chall_pnl / 100.0 : chall_pnl / 100.0;
    }

    return (chall_pnl - champ_pnl) / std::abs(champ_pnl);
}

} // namespace tb::champion_challenger
