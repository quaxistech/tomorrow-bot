/**
 * @file cost_attribution_engine.cpp
 * @brief Реализация декомпозиции PnL
 */
#include "cost_attribution/cost_attribution_engine.hpp"

#include <algorithm>
#include <cmath>

namespace tb::cost_attribution {

void CostAttributionEngine::record_trade(TradeCostBreakdown breakdown) {
    // Вычислить net если не задан
    breakdown.net_pnl_usdt = breakdown.gross_pnl_usdt
                              - breakdown.total_fee_usdt
                              - breakdown.slippage_usdt
                              - breakdown.funding_cost_usdt
                              - breakdown.missed_fill_cost_usdt;

    std::lock_guard lock(mutex_);
    trades_.push_back(std::move(breakdown));
}

CostAttributionSummary CostAttributionEngine::summarize(Timestamp from, Timestamp to) const {
    std::lock_guard lock(mutex_);
    std::vector<TradeCostBreakdown> filtered;
    for (const auto& t : trades_) {
        if (t.opened_at >= from && t.closed_at <= to && t.opened_at <= t.closed_at) {
            filtered.push_back(t);
        }
    }
    return aggregate(filtered);
}

CostAttributionSummary CostAttributionEngine::summarize_all() const {
    std::lock_guard lock(mutex_);
    return aggregate(trades_);
}

std::size_t CostAttributionEngine::trade_count() const {
    std::lock_guard lock(mutex_);
    return trades_.size();
}

void CostAttributionEngine::clear() {
    std::lock_guard lock(mutex_);
    trades_.clear();
}

CostAttributionSummary CostAttributionEngine::aggregate(
        const std::vector<TradeCostBreakdown>& trades) const {
    CostAttributionSummary summary;
    summary.trade_count = trades.size();

    if (trades.empty()) return summary;

    summary.period_start = trades.front().opened_at;
    summary.period_end = trades.back().closed_at;

    double slip_bps_sum = 0, cost_bps_sum = 0;

    for (const auto& t : trades) {
        summary.total_gross_pnl += t.gross_pnl_usdt;
        summary.total_maker_fees += t.maker_fee_usdt;
        summary.total_taker_fees += t.taker_fee_usdt;
        summary.total_fees += t.total_fee_usdt;
        summary.total_slippage += t.slippage_usdt;
        summary.total_funding += t.funding_cost_usdt;
        summary.total_missed_fills += t.missed_fill_cost_usdt;
        summary.total_net_pnl += t.net_pnl_usdt;

        slip_bps_sum += t.slippage_bps;
        cost_bps_sum += t.total_cost_bps;

        if (t.opened_at < summary.period_start) summary.period_start = t.opened_at;
        if (t.closed_at > summary.period_end) summary.period_end = t.closed_at;
    }

    // Percentages of absolute gross
    const double abs_gross = std::abs(summary.total_gross_pnl);
    if (abs_gross > 1e-10) {
        summary.fees_pct = (summary.total_fees / abs_gross) * 100.0;
        summary.slippage_pct = (summary.total_slippage / abs_gross) * 100.0;
        summary.funding_pct = (summary.total_funding / abs_gross) * 100.0;
        summary.missed_pct = (summary.total_missed_fills / abs_gross) * 100.0;
    }

    const double n = static_cast<double>(trades.size());
    summary.avg_slippage_bps = slip_bps_sum / n;
    summary.avg_total_cost_bps = cost_bps_sum / n;

    summary.trades = trades;
    return summary;
}

} // namespace tb::cost_attribution
