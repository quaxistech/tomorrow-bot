/**
 * @file champion_challenger_engine.cpp
 * @brief Реализация Champion-Challenger A/B тестирования (v2)
 *
 * Изменения vs v1:
 *  - PnLBreakdown: net_pnl = gross - fee - slippage (честное сравнение)
 *  - Drawdown tracking: peak/max_drawdown на каждой стороне
 *  - Hit rate: profitable_count / decision_count
 *  - Pre-promotion audit: pnl_delta + hit_rate + drawdown + regime_consistency
 *  - Metrics export: Prometheus gauges через IMetricsRegistry
 *  - Persistence: journal events через IStorageAdapter
 *  - Observer callbacks: on_promotion / on_rejection
 *  - Валидация входных данных
 */
#include "champion_challenger/champion_challenger_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::champion_challenger {

// ============================================================
// Конструктор
// ============================================================

ChampionChallengerEngine::ChampionChallengerEngine(
    ChampionChallengerConfig config,
    std::shared_ptr<logging::ILogger>             logger,
    std::shared_ptr<metrics::IMetricsRegistry>    metrics,
    std::shared_ptr<persistence::IStorageAdapter> storage)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , storage_(std::move(storage))
{}

// ============================================================
// Регистрация
// ============================================================

VoidResult ChampionChallengerEngine::register_challenger(
    StrategyId      champion,
    StrategyId      challenger,
    StrategyVersion version)
{
    std::lock_guard lock(mutex_);

    const auto& key = challenger.get();
    if (entries_.contains(key)) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    ChallengerEntry entry;
    entry.challenger_id = challenger;
    entry.champion_id   = champion;
    entry.version       = version;
    entry.status        = ChallengerStatus::Registered;

    entries_[key] = std::move(entry);

    if (logger_) {
        logger_->info("champion_challenger", "Challenger зарегистрирован",
            {{"champion",   champion.get()},
             {"challenger", challenger.get()},
             {"version",    std::to_string(version.get())}});
    }

    return OkVoid();
}

// ============================================================
// Запись результатов
// ============================================================

VoidResult ChampionChallengerEngine::record_champion_outcome(
    StrategyId         champion,
    PnLBreakdown       breakdown,
    const std::string& regime)
{
    std::lock_guard lock(mutex_);

    for (auto& [key, entry] : entries_) {
        if (entry.champion_id.get() != champion.get()) continue;
        if (entry.status == ChallengerStatus::Promoted  ||
            entry.status == ChallengerStatus::Rejected  ||
            entry.status == ChallengerStatus::Retired)  continue;

        if (entry.status == ChallengerStatus::Registered) {
            entry.status = ChallengerStatus::Evaluating;
        }

        const double net = breakdown.net_pnl_bps();
        auto& m = entry.champion_metrics;
        m.net_pnl_bps          += net;
        m.gross_pnl_bps        += breakdown.gross_pnl_bps;
        m.total_fee_bps        += breakdown.fee_bps;
        m.total_slippage_bps   += breakdown.slippage_bps;
        m.decision_count       += 1;
        m.regime_pnl[regime]   += net;
        m.regime_count[regime] += 1;
        if (net > 0.0) m.profitable_count += 1;
        update_drawdown(m);
    }

    return OkVoid();
}

VoidResult ChampionChallengerEngine::record_challenger_outcome(
    StrategyId         challenger,
    PnLBreakdown       breakdown,
    const std::string& regime,
    double             conviction)
{
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) {
        return ErrVoid(TbError::ConfigValidationFailed);
    }

    auto& entry = it->second;
    if (entry.status == ChallengerStatus::Promoted  ||
        entry.status == ChallengerStatus::Rejected  ||
        entry.status == ChallengerStatus::Retired)  {
        return OkVoid();  // молча игнорируем — статус финальный
    }

    if (entry.status == ChallengerStatus::Registered) {
        entry.status = ChallengerStatus::Evaluating;
    }

    // Клamp conviction в допустимый диапазон [0, 1]
    conviction = std::clamp(conviction, 0.0, 1.0);

    const double net = breakdown.net_pnl_bps();
    auto& m = entry.challenger_metrics;
    m.net_pnl_bps          += net;
    m.gross_pnl_bps        += breakdown.gross_pnl_bps;
    m.total_fee_bps        += breakdown.fee_bps;
    m.total_slippage_bps   += breakdown.slippage_bps;
    m.decision_count       += 1;
    m.regime_pnl[regime]   += net;
    m.regime_count[regime] += 1;
    if (net > 0.0) m.profitable_count += 1;

    // Инкрементальное обновление средней уверенности (Welford mean)
    m.avg_conviction += (conviction - m.avg_conviction) /
                        static_cast<double>(m.decision_count);

    update_drawdown(m);

    // Экспортируем метрики после каждого 10-го решения
    if (m.decision_count % 10 == 0) {
        export_metrics(entry);
    }

    return OkVoid();
}

// ============================================================
// Оценка
// ============================================================

Result<ChampionChallengerReport> ChampionChallengerEngine::evaluate(
    StrategyId champion) const
{
    std::lock_guard lock(mutex_);

    ChampionChallengerReport report;
    report.champion_id = champion;

    for (const auto& [key, entry] : entries_) {
        if (entry.champion_id.get() != champion.get()) continue;
        report.challengers.push_back(entry);
    }

    return Ok(std::move(report));
}

PrePromotionAudit ChampionChallengerEngine::audit_challenger(
    StrategyId challenger) const
{
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) {
        PrePromotionAudit fail;
        fail.failure_reason = "Challenger не найден: " + challenger.get();
        return fail;
    }
    return run_pre_promotion_audit(it->second);
}

bool ChampionChallengerEngine::should_promote(StrategyId challenger) const {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) return false;

    const auto& entry = it->second;
    if (entry.status != ChallengerStatus::Evaluating) return false;
    if (entry.challenger_metrics.decision_count < config_.min_evaluation_trades) return false;

    return run_pre_promotion_audit(entry).all_passed();
}

bool ChampionChallengerEngine::should_reject(StrategyId challenger) const {
    std::lock_guard lock(mutex_);

    auto it = entries_.find(challenger.get());
    if (it == entries_.end()) return false;

    const auto& entry = it->second;
    if (entry.status != ChallengerStatus::Evaluating) return false;
    if (entry.challenger_metrics.decision_count < config_.min_evaluation_trades) return false;

    return compute_performance_delta(entry) <= config_.rejection_threshold;
}

// ============================================================
// Действия
// ============================================================

VoidResult ChampionChallengerEngine::promote(StrategyId challenger, bool force) {
    ChallengerEntry entry_copy;
    std::vector<std::shared_ptr<IChallengerObserver>> observers_copy;

    {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(challenger.get());
        if (it == entries_.end()) {
            return ErrVoid(TbError::ConfigValidationFailed);
        }

        auto& entry = it->second;

        // Pre-promotion аудит (если не force)
        if (!force) {
            auto audit = run_pre_promotion_audit(entry);
            if (!audit.all_passed()) {
                if (logger_) {
                    logger_->warn("champion_challenger", "Промоушен отклонён аудитом",
                        {{"challenger",  challenger.get()},
                         {"reason",      audit.failure_reason}});
                }
                return ErrVoid(TbError::ConfigValidationFailed);
            }
        }

        double delta_pct = compute_performance_delta(entry) * 100.0;
        entry.status = ChallengerStatus::Promoted;
        entry.promotion_reason =
            "Net P&L delta +" + std::to_string(delta_pct) + "%" +
            " | hit_rate " + std::to_string(entry.challenger_metrics.hit_rate() * 100.0) + "%" +
            " | drawdown " + std::to_string(entry.challenger_metrics.max_drawdown_bps) + "bps" +
            (force ? " [forced]" : "");

        if (logger_) {
            logger_->info("champion_challenger", "CHALLENGER PROMOTED",
                {{"challenger",  challenger.get()},
                 {"champion",    entry.champion_id.get()},
                 {"delta_pct",   std::to_string(delta_pct)},
                 {"hit_rate",    std::to_string(entry.challenger_metrics.hit_rate())},
                 {"drawdown",    std::to_string(entry.challenger_metrics.max_drawdown_bps)},
                 {"trades",      std::to_string(entry.challenger_metrics.decision_count)},
                 {"forced",      force ? "true" : "false"}});
        }

        if (metrics_) {
            metrics::MetricTags tags{{"champion",   entry.champion_id.get()},
                                      {"challenger", challenger.get()}};
            metrics_->counter("cc_promotions_total", tags)->increment();
        }

        persist_event(challenger, "promoted", entry.promotion_reason);

        entry_copy = entry;
        observers_copy = observers_;
    }

    // Notify outside mutex to prevent deadlock if observer calls back into engine
    notify_promotion(entry_copy, observers_copy);
    return OkVoid();
}

VoidResult ChampionChallengerEngine::reject(StrategyId challenger) {
    ChallengerEntry entry_copy;
    std::vector<std::shared_ptr<IChallengerObserver>> observers_copy;

    {
        std::lock_guard lock(mutex_);

        auto it = entries_.find(challenger.get());
        if (it == entries_.end()) {
            return ErrVoid(TbError::ConfigValidationFailed);
        }

        auto& entry = it->second;
        double delta_pct = std::abs(compute_performance_delta(entry)) * 100.0;
        entry.status = ChallengerStatus::Rejected;
        entry.rejection_reason =
            "Net P&L delta -" + std::to_string(delta_pct) + "%" +
            " | hit_rate " + std::to_string(entry.challenger_metrics.hit_rate() * 100.0) + "%" +
            " | drawdown " + std::to_string(entry.challenger_metrics.max_drawdown_bps) + "bps";

        if (logger_) {
            logger_->warn("champion_challenger", "CHALLENGER REJECTED",
                {{"challenger",  challenger.get()},
                 {"champion",    entry.champion_id.get()},
                 {"delta_pct",   std::to_string(-delta_pct)},
                 {"hit_rate",    std::to_string(entry.challenger_metrics.hit_rate())},
                 {"drawdown",    std::to_string(entry.challenger_metrics.max_drawdown_bps)},
                 {"trades",      std::to_string(entry.challenger_metrics.decision_count)}});
        }

        if (metrics_) {
            metrics::MetricTags tags{{"champion",   entry.champion_id.get()},
                                      {"challenger", challenger.get()}};
            metrics_->counter("cc_rejections_total", tags)->increment();
        }

        persist_event(challenger, "rejected", entry.rejection_reason);

        entry_copy = entry;
        observers_copy = observers_;
    }

    // Notify outside mutex to prevent deadlock if observer calls back into engine
    notify_rejection(entry_copy, observers_copy);
    return OkVoid();
}

// ============================================================
// Observer
// ============================================================

void ChampionChallengerEngine::add_observer(
    std::shared_ptr<IChallengerObserver> observer)
{
    std::lock_guard lock(mutex_);
    observers_.push_back(std::move(observer));
}

// ============================================================
// Приватные методы
// ============================================================

double ChampionChallengerEngine::compute_performance_delta(
    const ChallengerEntry& entry) const
{
    const double champ = entry.champion_metrics.net_pnl_bps;
    const double chall = entry.challenger_metrics.net_pnl_bps;

    if (std::abs(champ) < 1e-9) {
        // Champion на нуле — нормируем на 100 bps как базу
        return chall / 100.0;
    }
    return (chall - champ) / std::abs(champ);
}

void ChampionChallengerEngine::update_drawdown(ComparisonMetrics& m) const {
    // Peak обновляется только вверх
    if (m.net_pnl_bps > m.peak_net_pnl_bps) {
        m.peak_net_pnl_bps = m.net_pnl_bps;
    }
    // Drawdown = текущее значение относительно пика (всегда ≤ 0)
    const double dd = m.net_pnl_bps - m.peak_net_pnl_bps;
    if (dd < m.max_drawdown_bps) {
        m.max_drawdown_bps = dd;
    }
}

PrePromotionAudit ChampionChallengerEngine::run_pre_promotion_audit(
    const ChallengerEntry& entry) const
{
    PrePromotionAudit audit;

    // --- 1. Net P&L delta ---
    double delta = compute_performance_delta(entry);
    audit.pnl_delta_passed = (delta >= config_.promotion_threshold);
    if (!audit.pnl_delta_passed) {
        audit.failure_reason = "Net P&L delta " + std::to_string(delta * 100.0) +
                               "% < порога " + std::to_string(config_.promotion_threshold * 100.0) + "%";
        return audit;  // ранний выход: основной критерий не выполнен
    }

    // --- 2. Hit rate ---
    double hit = entry.challenger_metrics.hit_rate();
    audit.hit_rate_adequate = (hit >= config_.min_hit_rate);
    if (!audit.hit_rate_adequate) {
        audit.failure_reason = "Hit rate " + std::to_string(hit * 100.0) +
                               "% < минимума " + std::to_string(config_.min_hit_rate * 100.0) + "%";
        return audit;
    }

    // --- 3. Max drawdown ---
    double dd = entry.challenger_metrics.max_drawdown_bps;
    audit.max_drawdown_acceptable = (dd >= config_.max_drawdown_bps);
    if (!audit.max_drawdown_acceptable) {
        audit.failure_reason = "Max drawdown " + std::to_string(dd) +
                               "bps хуже лимита " + std::to_string(config_.max_drawdown_bps) + "bps";
        return audit;
    }

    // --- 4. Regime consistency ---
    int regimes_tested = 0;
    int regimes_won    = 0;
    for (const auto& [regime, chall_pnl] : entry.challenger_metrics.regime_pnl) {
        auto cnt_it = entry.challenger_metrics.regime_count.find(regime);
        if (cnt_it == entry.challenger_metrics.regime_count.end() ||
            cnt_it->second < config_.min_regime_samples) continue;
        ++regimes_tested;

        auto champ_it = entry.champion_metrics.regime_pnl.find(regime);
        double champ_pnl = (champ_it != entry.champion_metrics.regime_pnl.end())
                            ? champ_it->second : 0.0;
        if (chall_pnl >= champ_pnl) ++regimes_won;
    }

    // Без достаточного числа режимов — пропускаем проверку
    audit.regime_consistency_passed = (regimes_tested == 0) ||
        (static_cast<double>(regimes_won) / regimes_tested >= 0.60);

    if (!audit.regime_consistency_passed) {
        audit.failure_reason = "Regime consistency: выиграл " +
            std::to_string(regimes_won) + "/" + std::to_string(regimes_tested) +
            " режимов (нужно ≥60%)";
    }

    return audit;
}

void ChampionChallengerEngine::export_metrics(const ChallengerEntry& entry) const {
    if (!metrics_) return;

    const auto& cm = entry.challenger_metrics;
    metrics::MetricTags tags{
        {"champion",   entry.champion_id.get()},
        {"challenger", entry.challenger_id.get()}
    };

    metrics_->gauge("cc_challenger_net_pnl_bps",    tags)->set(cm.net_pnl_bps);
    metrics_->gauge("cc_challenger_gross_pnl_bps",  tags)->set(cm.gross_pnl_bps);
    metrics_->gauge("cc_challenger_fee_bps",         tags)->set(cm.total_fee_bps);
    metrics_->gauge("cc_challenger_slippage_bps",    tags)->set(cm.total_slippage_bps);
    metrics_->gauge("cc_challenger_hit_rate",         tags)->set(cm.hit_rate());
    metrics_->gauge("cc_challenger_drawdown_bps",    tags)->set(cm.max_drawdown_bps);
    metrics_->gauge("cc_challenger_conviction",      tags)->set(cm.avg_conviction);
    metrics_->gauge("cc_challenger_trade_count",     tags)->set(
        static_cast<double>(cm.decision_count));

    metrics_->gauge("cc_performance_delta",          tags)->set(
        compute_performance_delta(entry));

    // Статус как числовой gauge: Registered=0, Evaluating=1, Promoted=2,
    //                             Rejected=3, Retired=4
    metrics_->gauge("cc_challenger_status",          tags)->set(
        static_cast<double>(entry.status));
}

void ChampionChallengerEngine::persist_event(
    const StrategyId&  challenger_id,
    const std::string& event_type,
    const std::string& payload_json) const
{
    if (!storage_) return;

    persistence::JournalEntry je;
    je.type         = persistence::JournalEntryType::StrategySignal;
    je.strategy_id  = challenger_id;
    je.payload_json = "cc:" + event_type + ":" + payload_json;
    (void)storage_->append_journal(je);  // best-effort
}

void ChampionChallengerEngine::notify_promotion(
    const ChallengerEntry& entry,
    const std::vector<std::shared_ptr<IChallengerObserver>>& observers) {
    for (const auto& obs : observers) {
        if (obs) obs->on_promotion(entry);
    }
}

void ChampionChallengerEngine::notify_rejection(
    const ChallengerEntry& entry,
    const std::vector<std::shared_ptr<IChallengerObserver>>& observers) {
    for (const auto& obs : observers) {
        if (obs) obs->on_rejection(entry);
    }
}

} // namespace tb::champion_challenger
