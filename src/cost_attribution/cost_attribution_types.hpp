#pragma once
/**
 * @file cost_attribution_types.hpp
 * @brief Типы для декомпозиции PnL: gross alpha → fees → slippage → funding → missed fills
 */
#include "common/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tb::cost_attribution {

// ============================================================
// Per-trade decomposition
// ============================================================

struct TradeCostBreakdown {
    std::string trade_id;
    Symbol symbol{Symbol{""}};
    Timestamp opened_at{Timestamp{0}};
    Timestamp closed_at{Timestamp{0}};

    double gross_pnl_usdt{0.0};           ///< P&L до всех издержек
    double maker_fee_usdt{0.0};            ///< комиссия maker
    double taker_fee_usdt{0.0};            ///< комиссия taker
    double total_fee_usdt{0.0};            ///< суммарная комиссия
    double slippage_usdt{0.0};             ///< проскальзывание
    double funding_cost_usdt{0.0};         ///< оплата фандинга за время удержания
    double missed_fill_cost_usdt{0.0};     ///< упущенная прибыль от не-заполненных ордеров
    double net_pnl_usdt{0.0};             ///< = gross - fees - slippage - funding - missed

    double slippage_bps{0.0};
    double total_cost_bps{0.0};
    double net_pnl_bps{0.0};
};

// ============================================================
// Aggregate
// ============================================================

struct CostAttributionSummary {
    std::size_t trade_count{0};
    Timestamp period_start{Timestamp{0}};
    Timestamp period_end{Timestamp{0}};

    double total_gross_pnl{0.0};
    double total_maker_fees{0.0};
    double total_taker_fees{0.0};
    double total_fees{0.0};
    double total_slippage{0.0};
    double total_funding{0.0};
    double total_missed_fills{0.0};
    double total_net_pnl{0.0};

    // Percentages of gross
    double fees_pct{0.0};
    double slippage_pct{0.0};
    double funding_pct{0.0};
    double missed_pct{0.0};

    // Averages per trade
    double avg_slippage_bps{0.0};
    double avg_total_cost_bps{0.0};

    std::vector<TradeCostBreakdown> trades;
};

} // namespace tb::cost_attribution
