#pragma once
/**
 * @file execution_metrics.hpp
 * @brief Execution quality monitoring & telemetry (§23 ТЗ)
 *
 * Отслеживает: latency, slippage, reject/cancel rates, recovery frequency.
 */

#include "execution/order_types.hpp"
#include "execution/execution_types.hpp"
#include "metrics/metrics_registry.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <mutex>

namespace tb::execution {

/// Агрегированная статистика исполнения
struct ExecutionStats {
    int64_t total_orders{0};
    int64_t filled_orders{0};
    int64_t rejected_orders{0};
    int64_t cancelled_orders{0};
    int64_t timed_out_orders{0};
    int64_t recovery_events{0};
    double avg_submit_to_fill_ms{0.0};
    double avg_slippage_bps{0.0};
    double total_fees_usdt{0.0};
    double fill_rate_pct{0.0};              ///< filled / total * 100
    double reject_rate_pct{0.0};
    double cancel_rate_pct{0.0};

    // ── Execution quality feedback (Phase 6) ──
    int64_t passive_submissions{0};          ///< Total limit order submissions
    int64_t passive_fills{0};                ///< Limit orders that got filled
    double passive_fill_rate_pct{0.0};       ///< passive_fills / passive_submissions * 100
    double cancel_to_fill_ratio{0.0};        ///< cancelled / filled
    double adverse_selection_mean_bps{0.0};  ///< Avg adverse price move after fill
};

/// Execution Metrics Collector
class ExecutionMetrics {
public:
    explicit ExecutionMetrics(std::shared_ptr<metrics::IMetricsRegistry> metrics,
                              std::shared_ptr<logging::ILogger> logger);

    /// Записать отправку ордера
    void record_submission(const OrderRecord& order);

    /// Записать fill (с latency и fee)
    void record_fill(const OrderRecord& order, Price fill_price,
                     int64_t latency_ms, double fee_usdt = 0.0);

    /// Записать reject
    void record_rejection(const OrderRecord& order, const std::string& reason);

    /// Записать cancel
    void record_cancel(const OrderRecord& order);

    /// Записать timeout
    void record_timeout(const OrderRecord& order);

    /// Записать recovery event
    void record_recovery(const std::string& detail);

    /// Записать slippage
    void record_slippage(const Symbol& symbol, double expected_price,
                         double actual_price, Side side);

    /// Получить текущую статистику (thread-safe snapshot)
    ExecutionStats snapshot() const;

    /// Сбросить статистику (для daily reset)
    void reset();

private:
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<logging::ILogger> logger_;

    mutable std::mutex mutex_;
    ExecutionStats stats_;
    double slippage_sum_bps_{0.0};
    int64_t slippage_count_{0};
    double latency_sum_ms_{0.0};
    int64_t latency_count_{0};
};

} // namespace tb::execution
