/**
 * @file execution_quality_monitor.cpp
 * @brief Реализация ExecutionQualityMonitor
 */

#include "execution/execution_quality_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::execution {

// ── SymbolAccum helpers ──

void ExecutionQualityMonitor::SymbolAccum::push_fill_latency(double ms) {
    if (fill_latency_top_.size() < kTopN) {
        fill_latency_top_.push_back(ms);
        std::sort(fill_latency_top_.begin(), fill_latency_top_.end());
    } else if (ms > fill_latency_top_.front()) {
        fill_latency_top_.front() = ms;
        std::sort(fill_latency_top_.begin(), fill_latency_top_.end());
    }
}

// ── Constructor ──

ExecutionQualityMonitor::ExecutionQualityMonitor(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        hist_fill_latency_ = metrics_->histogram(
            "tb_exec_fill_latency_ms",
            {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2000, 5000}, {});
        hist_cancel_latency_ = metrics_->histogram(
            "tb_exec_cancel_latency_ms",
            {1, 5, 10, 25, 50, 100, 250, 500, 1000}, {});
        hist_slippage_ = metrics_->histogram(
            "tb_exec_slippage_bps",
            {-5, -2, -1, -0.5, 0, 0.5, 1, 2, 5, 10, 20}, {});
    }
}

// ── Events ──

void ExecutionQualityMonitor::on_fill(
    const Symbol& symbol, int64_t latency_ns,
    double expected_price, double actual_price, Side side)
{
    double latency_ms = static_cast<double>(latency_ns) / 1e6;

    // Slippage: positive = unfavorable
    double slippage_bps = 0.0;
    if (expected_price > 0.0) {
        double raw = (actual_price - expected_price) / expected_price * 10000.0;
        slippage_bps = (side == Side::Buy) ? raw : -raw;
    }

    std::lock_guard lock(mutex_);
    auto& a = accum_[symbol.get()];
    a.fills++;
    a.fill_latency_sum_ms += latency_ms;
    if (latency_ms > a.fill_latency_max_ms) a.fill_latency_max_ms = latency_ms;
    a.slippage_sum_bps += slippage_bps;
    a.push_fill_latency(latency_ms);

    if (hist_fill_latency_) hist_fill_latency_->observe(latency_ms);
    if (hist_slippage_) hist_slippage_->observe(slippage_bps);
}

void ExecutionQualityMonitor::on_cancel(
    const Symbol& symbol, int64_t latency_ns)
{
    double latency_ms = static_cast<double>(latency_ns) / 1e6;

    std::lock_guard lock(mutex_);
    auto& a = accum_[symbol.get()];
    a.cancels++;
    a.cancel_latency_sum_ms += latency_ms;

    if (hist_cancel_latency_) hist_cancel_latency_->observe(latency_ms);
}

void ExecutionQualityMonitor::on_missed_liquidity(
    const Symbol& symbol, double price_moved_bps)
{
    std::lock_guard lock(mutex_);
    accum_[symbol.get()].missed_liquidity_events++;

    if (logger_ && price_moved_bps > 5.0) {
        logger_->warn("ExecQuality", "missed_liquidity", {
            {"symbol", symbol.get()},
            {"price_moved_bps", std::to_string(price_moved_bps)}
        });
    }
}

void ExecutionQualityMonitor::on_queue_loss(const Symbol& symbol) {
    std::lock_guard lock(mutex_);
    accum_[symbol.get()].queue_loss_events++;
}

void ExecutionQualityMonitor::on_passive_submit(const Symbol& symbol) {
    std::lock_guard lock(mutex_);
    accum_[symbol.get()].passive_submissions++;
}

// ── Queries ──

SymbolExecQuality ExecutionQualityMonitor::quality_for(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = accum_.find(symbol.get());
    if (it == accum_.end()) return {symbol.get()};
    return compute_quality(it->second, it->first);
}

SymbolExecQuality ExecutionQualityMonitor::aggregate_quality() const {
    std::lock_guard lock(mutex_);

    SymbolAccum total{};
    for (const auto& [sym, a] : accum_) {
        total.fills += a.fills;
        total.cancels += a.cancels;
        total.passive_submissions += a.passive_submissions;
        total.missed_liquidity_events += a.missed_liquidity_events;
        total.queue_loss_events += a.queue_loss_events;
        total.fill_latency_sum_ms += a.fill_latency_sum_ms;
        total.cancel_latency_sum_ms += a.cancel_latency_sum_ms;
        total.slippage_sum_bps += a.slippage_sum_bps;
        if (a.fill_latency_max_ms > total.fill_latency_max_ms)
            total.fill_latency_max_ms = a.fill_latency_max_ms;
        for (double v : a.fill_latency_top_)
            total.push_fill_latency(v);
    }
    return compute_quality(total, "ALL");
}

void ExecutionQualityMonitor::reset() {
    std::lock_guard lock(mutex_);
    accum_.clear();
}

// ── Internal ──

SymbolExecQuality ExecutionQualityMonitor::compute_quality(
    const SymbolAccum& a, const std::string& sym) const
{
    SymbolExecQuality q;
    q.symbol = sym;
    q.fills = a.fills;
    q.cancels = a.cancels;

    if (a.fills > 0) {
        q.avg_fill_latency_ms = a.fill_latency_sum_ms / static_cast<double>(a.fills);
        q.avg_slippage_bps = a.slippage_sum_bps / static_cast<double>(a.fills);
    }
    if (a.cancels > 0) {
        q.avg_cancel_latency_ms = a.cancel_latency_sum_ms / static_cast<double>(a.cancels);
    }

    // P99 from top-N reservoir
    if (!a.fill_latency_top_.empty()) {
        auto idx = static_cast<std::size_t>(
            std::ceil(0.99 * static_cast<double>(a.fill_latency_top_.size())) - 1);
        idx = std::min(idx, a.fill_latency_top_.size() - 1);
        q.p99_fill_latency_ms = a.fill_latency_top_[idx];
    }

    // Missed liquidity as % of fills
    int64_t total_events = a.fills + a.missed_liquidity_events;
    if (total_events > 0) {
        q.missed_liquidity_pct =
            100.0 * static_cast<double>(a.missed_liquidity_events)
            / static_cast<double>(total_events);
    }

    // Queue loss as % of passive submissions
    if (a.passive_submissions > 0) {
        q.queue_loss_pct =
            100.0 * static_cast<double>(a.queue_loss_events)
            / static_cast<double>(a.passive_submissions);
    }

    return q;
}

} // namespace tb::execution
