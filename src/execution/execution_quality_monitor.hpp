#pragma once
/**
 * @file execution_quality_monitor.hpp
 * @brief Histogram-based execution quality monitoring
 *
 * Tracks fill latency, cancel latency, slippage vs expected,
 * missed liquidity, and queue loss distributions via Prometheus histograms.
 * Complements ExecutionMetrics (counters) with distribution-level insight.
 */

#include "common/types.hpp"
#include "metrics/metrics_registry.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tb::execution {

/// Per-symbol execution quality snapshot
struct SymbolExecQuality {
    std::string symbol;
    int64_t fills{0};
    int64_t cancels{0};
    double avg_fill_latency_ms{0.0};
    double p99_fill_latency_ms{0.0};
    double avg_cancel_latency_ms{0.0};
    double avg_slippage_bps{0.0};
    double missed_liquidity_pct{0.0};     ///< % times best bid/ask moved away before fill
    double queue_loss_pct{0.0};           ///< % passive orders that lost queue position
};

class ExecutionQualityMonitor {
public:
    ExecutionQualityMonitor(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    /// Record fill latency (ns from order submit to first fill)
    void on_fill(const Symbol& symbol, int64_t latency_ns,
                 double expected_price, double actual_price, Side side);

    /// Record cancel latency (ns from cancel request to ack)
    void on_cancel(const Symbol& symbol, int64_t latency_ns);

    /// Record missed liquidity event (best price moved before fill)
    void on_missed_liquidity(const Symbol& symbol, double price_moved_bps);

    /// Record queue loss (passive order lost priority)
    void on_queue_loss(const Symbol& symbol);

    /// Record passive order submission (limit/post-only)
    void on_passive_submit(const Symbol& symbol);

    /// Get per-symbol quality snapshot
    [[nodiscard]] SymbolExecQuality quality_for(const Symbol& symbol) const;

    /// Get aggregated quality across all symbols
    [[nodiscard]] SymbolExecQuality aggregate_quality() const;

    /// Reset all tracking
    void reset();

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    // Per-symbol accumulators
    struct SymbolAccum {
        int64_t fills{0};
        int64_t cancels{0};
        int64_t passive_submissions{0};
        int64_t missed_liquidity_events{0};
        int64_t queue_loss_events{0};
        double fill_latency_sum_ms{0.0};
        double fill_latency_max_ms{0.0};
        double cancel_latency_sum_ms{0.0};
        double slippage_sum_bps{0.0};
        // Reservoir for P99 approximation (sorted top-N)
        std::vector<double> fill_latency_top_;
        static constexpr std::size_t kTopN = 100;
        void push_fill_latency(double ms);
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SymbolAccum> accum_;

    // Prometheus histograms
    std::shared_ptr<metrics::IHistogram> hist_fill_latency_;
    std::shared_ptr<metrics::IHistogram> hist_cancel_latency_;
    std::shared_ptr<metrics::IHistogram> hist_slippage_;

    SymbolExecQuality compute_quality(const SymbolAccum& a, const std::string& sym) const;
};

} // namespace tb::execution
