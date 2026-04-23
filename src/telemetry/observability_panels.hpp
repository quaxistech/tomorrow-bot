#pragma once
/**
 * @file observability_panels.hpp
 * @brief Metrics for the 7 required observability panels
 *
 * Panels:
 * 1. Fill duplication — duplicate tradeId counter
 * 2. Reconciliation mismatches — already in reconciliation_engine
 * 3. Orphan legs — unmatched hedge legs
 * 4. Stale feed windows — market data age histogram
 * 5. Pair EV vs realized pair PnL — predicted vs actual
 * 6. Regime mismatch frequency — regime prediction misses
 * 7. Funding drag — cumulative funding cost
 */
#include "metrics/metrics_registry.hpp"
#include <memory>

namespace tb::telemetry {

struct ObservabilityPanels {
    // 1. Fill duplication
    std::shared_ptr<metrics::ICounter> fill_duplicates_total;

    // 2. Reconciliation mismatches (already tracked in reconciliation_engine)
    // — metrics_->counter("reconciliation_mismatches_total") already exists

    // 3. Orphan legs
    std::shared_ptr<metrics::IGauge> orphan_legs_active;
    std::shared_ptr<metrics::ICounter> orphan_legs_total;

    // 4. Stale feed windows
    std::shared_ptr<metrics::IHistogram> feed_age_ms;
    std::shared_ptr<metrics::ICounter> stale_ticks_total;

    // 5. Pair EV vs realized PnL
    std::shared_ptr<metrics::IHistogram> pair_predicted_ev_bps;
    std::shared_ptr<metrics::IHistogram> pair_realized_pnl_bps;

    // 6. Regime mismatch
    std::shared_ptr<metrics::ICounter> regime_mismatch_total;
    std::shared_ptr<metrics::ICounter> regime_correct_total;

    // 7. Funding drag
    std::shared_ptr<metrics::IHistogram> funding_rate_bps;
    std::shared_ptr<metrics::ICounter> funding_drag_cumulative_bps;

    static ObservabilityPanels create(
        const std::shared_ptr<metrics::IMetricsRegistry>& metrics) {
        ObservabilityPanels p;
        if (!metrics) return p;

        p.fill_duplicates_total = metrics->counter("tb_fill_duplicates_total");

        p.orphan_legs_active = metrics->gauge("tb_orphan_legs_active");
        p.orphan_legs_total = metrics->counter("tb_orphan_legs_total");

        p.feed_age_ms = metrics->histogram("tb_feed_age_ms",
            {1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0});
        p.stale_ticks_total = metrics->counter("tb_stale_ticks_total");

        p.pair_predicted_ev_bps = metrics->histogram("tb_pair_predicted_ev_bps",
            {-20.0, -10.0, -5.0, 0.0, 5.0, 10.0, 20.0, 50.0});
        p.pair_realized_pnl_bps = metrics->histogram("tb_pair_realized_pnl_bps",
            {-50.0, -20.0, -10.0, 0.0, 10.0, 20.0, 50.0, 100.0});

        p.regime_mismatch_total = metrics->counter("tb_regime_mismatch_total");
        p.regime_correct_total = metrics->counter("tb_regime_correct_total");

        p.funding_rate_bps = metrics->histogram("tb_funding_rate_bps",
            {-5.0, -2.0, -1.0, 0.0, 1.0, 2.0, 5.0, 10.0});
        p.funding_drag_cumulative_bps = metrics->counter("tb_funding_drag_cumulative_bps");

        return p;
    }
};

} // namespace tb::telemetry
