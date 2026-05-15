/**
 * @file pair_economics.cpp
 * @brief Реализация PairEconomicsTracker
 */

#include "pipeline/pair_economics.hpp"
#include "logging/log_context.hpp"

#include <algorithm>
#include <cmath>

namespace tb::pipeline {

PairEconomicsTracker::PairEconomicsTracker(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        counter_pair_cycles_ = metrics_->counter("tb_pair_cycles_total", {});
        gauge_net_pair_pnl_ = metrics_->gauge("tb_pair_net_pnl_usd", {});
        hist_exit_efficiency_ = metrics_->histogram(
            "tb_pair_exit_efficiency",
            {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}, {});
        hist_time_in_pair_ = metrics_->histogram(
            "tb_pair_time_seconds",
            {5, 15, 30, 60, 120, 300, 600, 1800, 3600}, {});
        counter_total_fees_ = metrics_->counter("tb_pair_fees_total_usd", {});
        counter_total_funding_ = metrics_->counter("tb_pair_funding_total_usd", {});
    }
}

void PairEconomicsTracker::record(const PairEconomicsRecord& rec) {
    if (logger_) {
        logger_->info("PairEconomics", "pair_cycle_complete", {
            {"symbol", rec.symbol.get()},
            {"gross_pnl", std::to_string(rec.gross_pair_pnl)},
            {"net_pnl", std::to_string(rec.net_pair_pnl)},
            {"fees", std::to_string(rec.total_fees)},
            {"funding", std::to_string(rec.total_funding)},
            {"slippage", std::to_string(rec.total_slippage)},
            {"exit_efficiency", std::to_string(rec.exit_efficiency)},
            {"time_in_pair_sec", std::to_string(
                static_cast<double>(rec.time_in_pair_ns) / 1e9)},
            {"hedge_ratio", std::to_string(rec.hedge_ratio_actual)},
            {"primary_exit", std::string(to_string(rec.primary_exit_reason))},
            {"hedge_exit", std::string(to_string(rec.hedge_exit_reason))},
            {"reason_code_primary", std::to_string(code_value(rec.primary_exit_reason))},
            {"reason_code_hedge", std::to_string(code_value(rec.hedge_exit_reason))}
        });
    }

    std::lock_guard lock(mutex_);

    // Update daily stats
    daily_.pair_cycles++;
    daily_.total_gross_pnl += rec.gross_pair_pnl;
    daily_.total_net_pnl += rec.net_pair_pnl;
    daily_.total_fees += rec.total_fees;
    daily_.total_funding += rec.total_funding;
    daily_.total_slippage += rec.total_slippage;

    // Running average for exit efficiency
    daily_.avg_exit_efficiency +=
        (rec.exit_efficiency - daily_.avg_exit_efficiency) / daily_.pair_cycles;

    double time_sec = static_cast<double>(rec.time_in_pair_ns) / 1e9;
    daily_.avg_time_in_pair_sec +=
        (time_sec - daily_.avg_time_in_pair_sec) / daily_.pair_cycles;

    // BUG-S32-05: track wins as integer to avoid float rounding error accumulation
    if (rec.net_pair_pnl > 0.0) daily_.win_count++;
    daily_.win_rate = static_cast<double>(daily_.win_count) / daily_.pair_cycles;

    // Store record
    records_.push_back(rec);
    if (records_.size() > kMaxRecords) {
        records_.pop_front();
    }

    // Prometheus
    if (counter_pair_cycles_) counter_pair_cycles_->increment();
    if (gauge_net_pair_pnl_) gauge_net_pair_pnl_->set(daily_.total_net_pnl);
    if (hist_exit_efficiency_) hist_exit_efficiency_->observe(rec.exit_efficiency);
    if (hist_time_in_pair_) hist_time_in_pair_->observe(time_sec);
    if (counter_total_fees_) counter_total_fees_->increment(rec.total_fees);
    if (counter_total_funding_) counter_total_funding_->increment(std::abs(rec.total_funding));
}

std::vector<PairEconomicsRecord> PairEconomicsTracker::recent(std::size_t n) const {
    std::lock_guard lock(mutex_);
    std::size_t count = std::min(n, records_.size());
    return {records_.end() - static_cast<std::ptrdiff_t>(count), records_.end()};
}

PairEconomicsTracker::DailyStats PairEconomicsTracker::daily_stats() const {
    std::lock_guard lock(mutex_);
    return daily_;
}

void PairEconomicsTracker::reset_daily() {
    std::lock_guard lock(mutex_);
    daily_ = {};
}

} // namespace tb::pipeline
