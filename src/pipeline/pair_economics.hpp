#pragma once
/**
 * @file pair_economics.hpp
 * @brief Трекер экономики хедж-пар: gross/net PnL, fees, funding, slippage, exit quality
 *
 * Агрегирует метрики по парным позициям (primary + hedge) для observability.
 * Записывает каждый завершённый pair-цикл как PairEconomicsRecord.
 * Экспортирует метрики через IMetricsRegistry (Prometheus).
 */

#include "common/types.hpp"
#include "common/reason_codes.hpp"
#include "metrics/metrics_registry.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace tb::pipeline {

// ============================================================
// Запись экономики одного pair-цикла
// ============================================================

struct PairEconomicsRecord {
    Symbol symbol{""};
    StrategyId strategy_id{""};
    CorrelationId correlation_id{""};

    // PnL
    double primary_gross_pnl{0.0};      ///< Gross PnL primary ноги
    double hedge_gross_pnl{0.0};        ///< Gross PnL hedge ноги
    double gross_pair_pnl{0.0};         ///< primary + hedge (до fees)
    double net_pair_pnl{0.0};           ///< После fees и funding

    // Costs
    double total_fees{0.0};             ///< Taker/maker fees обеих ног
    double total_funding{0.0};          ///< Accumulated funding обеих ног
    double total_slippage{0.0};         ///< Суммарный slippage (bps → USD)

    // Timing
    int64_t time_in_pair_ns{0};         ///< Время жизни пары
    int64_t primary_hold_ns{0};         ///< Время удержания primary
    int64_t hedge_hold_ns{0};           ///< Время удержания hedge

    // Exit quality
    double exit_efficiency{0.0};        ///< net / max_potential (0..1)
    ReasonCode primary_exit_reason{ReasonCode::ExitTakeProfit};
    ReasonCode hedge_exit_reason{ReasonCode::HedgeCloseProfitLock};

    // Sizes
    double primary_size{0.0};
    double hedge_size{0.0};
    double hedge_ratio_actual{0.0};     ///< hedge_size / primary_size

    Timestamp opened_at{0};
    Timestamp closed_at{0};
};

// ============================================================
// Трекер
// ============================================================

class PairEconomicsTracker {
public:
    PairEconomicsTracker(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    /// Записать завершённый pair-цикл
    void record(const PairEconomicsRecord& rec);

    /// Получить последние N записей
    [[nodiscard]] std::vector<PairEconomicsRecord> recent(std::size_t n = 50) const;

    /// Агрегированные метрики за текущий день
    struct DailyStats {
        int pair_cycles{0};
        double total_gross_pnl{0.0};
        double total_net_pnl{0.0};
        double total_fees{0.0};
        double total_funding{0.0};
        double total_slippage{0.0};
        double avg_exit_efficiency{0.0};
        double avg_time_in_pair_sec{0.0};
        double win_rate{0.0};           ///< % pairs with net_pair_pnl > 0
    };

    [[nodiscard]] DailyStats daily_stats() const;

    /// Сброс дневных агрегатов
    void reset_daily();

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    mutable std::mutex mutex_;
    std::deque<PairEconomicsRecord> records_;
    static constexpr std::size_t kMaxRecords = 500;

    // Daily accumulators
    DailyStats daily_;

    // Metrics handles
    std::shared_ptr<metrics::ICounter> counter_pair_cycles_;
    std::shared_ptr<metrics::IGauge> gauge_net_pair_pnl_;
    std::shared_ptr<metrics::IHistogram> hist_exit_efficiency_;
    std::shared_ptr<metrics::IHistogram> hist_time_in_pair_;
    std::shared_ptr<metrics::ICounter> counter_total_fees_;
    std::shared_ptr<metrics::ICounter> counter_total_funding_;
};

} // namespace tb::pipeline
