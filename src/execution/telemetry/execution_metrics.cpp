#include "execution/telemetry/execution_metrics.hpp"
#include <cmath>

namespace tb::execution {

ExecutionMetrics::ExecutionMetrics(
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<logging::ILogger> logger)
    : metrics_(std::move(metrics))
    , logger_(std::move(logger))
{
}

void ExecutionMetrics::record_submission(const OrderRecord& order) {
    std::lock_guard lock(mutex_);
    stats_.total_orders++;

    if (metrics_) {
        metrics_->counter("execution_orders_submitted", {
            {"symbol", order.symbol.get()},
            {"side", order.side == Side::Buy ? "buy" : "sell"}
        })->increment();
    }
}

void ExecutionMetrics::record_fill(const OrderRecord& order,
                                    Price fill_price, int64_t latency_ms,
                                    double fee_usdt) {
    std::lock_guard lock(mutex_);
    stats_.filled_orders++;

    latency_sum_ms_ += static_cast<double>(latency_ms);
    latency_count_++;
    stats_.avg_submit_to_fill_ms = (latency_count_ > 0)
        ? latency_sum_ms_ / static_cast<double>(latency_count_) : 0.0;

    stats_.total_fees_usdt += fee_usdt;

    // Update fill rate
    if (stats_.total_orders > 0) {
        stats_.fill_rate_pct = 100.0 * static_cast<double>(stats_.filled_orders)
                               / static_cast<double>(stats_.total_orders);
    }

    if (metrics_) {
        metrics_->counter("execution_orders_filled", {
            {"symbol", order.symbol.get()}
        })->increment();
    }
}

void ExecutionMetrics::record_rejection(const OrderRecord& order,
                                         const std::string& reason) {
    std::lock_guard lock(mutex_);
    stats_.rejected_orders++;

    if (stats_.total_orders > 0) {
        stats_.reject_rate_pct = 100.0 * static_cast<double>(stats_.rejected_orders)
                                 / static_cast<double>(stats_.total_orders);
    }

    if (metrics_) {
        metrics_->counter("execution_orders_rejected", {
            {"symbol", order.symbol.get()},
            {"reason", reason}
        })->increment();
    }

    logger_->warn("ExecutionMetrics", "Order rejected",
        {{"order_id", order.order_id.get()},
         {"symbol", order.symbol.get()},
         {"reason", reason}});
}

void ExecutionMetrics::record_cancel(const OrderRecord& order) {
    std::lock_guard lock(mutex_);
    stats_.cancelled_orders++;

    if (stats_.total_orders > 0) {
        stats_.cancel_rate_pct = 100.0 * static_cast<double>(stats_.cancelled_orders)
                                 / static_cast<double>(stats_.total_orders);
    }

    if (metrics_) {
        metrics_->counter("execution_orders_cancelled", {
            {"symbol", order.symbol.get()}
        })->increment();
    }
}

void ExecutionMetrics::record_timeout(const OrderRecord& order) {
    std::lock_guard lock(mutex_);
    stats_.timed_out_orders++;

    if (metrics_) {
        metrics_->counter("execution_orders_timed_out", {
            {"symbol", order.symbol.get()}
        })->increment();
    }
}

void ExecutionMetrics::record_recovery(const std::string& detail) {
    std::lock_guard lock(mutex_);
    stats_.recovery_events++;

    if (metrics_) {
        metrics_->counter("execution_recovery_events", {})->increment();
    }

    logger_->warn("ExecutionMetrics", "Recovery event recorded",
        {{"detail", detail}});
}

void ExecutionMetrics::record_slippage(const Symbol& symbol,
                                        double expected, double actual, Side side) {
    if (expected <= 0.0) return;

    double slip_bps = ((actual - expected) / expected) * 10000.0;
    // For sells, positive slippage means worse execution
    if (side == Side::Sell) slip_bps = -slip_bps;

    std::lock_guard lock(mutex_);
    slippage_sum_bps_ += std::abs(slip_bps);
    slippage_count_++;
    stats_.avg_slippage_bps = (slippage_count_ > 0)
        ? slippage_sum_bps_ / static_cast<double>(slippage_count_) : 0.0;

    if (metrics_) {
        metrics_->gauge("execution_slippage_bps", {
            {"symbol", symbol.get()}
        })->set(slip_bps);
    }
}

ExecutionStats ExecutionMetrics::snapshot() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void ExecutionMetrics::reset() {
    std::lock_guard lock(mutex_);
    stats_ = ExecutionStats{};
    slippage_sum_bps_ = 0.0;
    slippage_count_ = 0;
    latency_sum_ms_ = 0.0;
    latency_count_ = 0;
}

} // namespace tb::execution
