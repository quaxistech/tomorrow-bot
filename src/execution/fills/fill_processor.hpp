#pragma once
/**
 * @file fill_processor.hpp
 * @brief Обработка fill events (§17 ТЗ)
 *
 * Идемпотентная обработка partial/full fills.
 * Обновляет OrderRecord, портфель, генерирует метрики.
 */

#include "execution/order_types.hpp"
#include "execution/orders/order_registry.hpp"
#include "execution/execution_config.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "persistence/event_journal.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>

namespace tb::execution {

/// Обработчик fill events
class FillProcessor {
public:
    FillProcessor(OrderRegistry& registry,
                  std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
                  std::shared_ptr<logging::ILogger> logger,
                  std::shared_ptr<clock::IClock> clock,
                  std::shared_ptr<metrics::IMetricsRegistry> metrics,
                  const ExecutionConfig& config);

    /// Обработать fill event для рыночного ордера (сразу после submit)
    /// Обновляет OrderRecord, FSM, портфель.
    /// @return true если fill применён, false если дубликат/ошибка
    bool process_market_fill(const OrderId& order_id,
                             Quantity filled_qty, Price fill_price,
                             const OrderId& exchange_order_id);

    /// Обработать внешний fill event (от биржи)
    /// @return true если fill применён
    bool process_fill_event(const FillEvent& fill);

    /// Обработать on_order_update (fill при Filled → обновить portfolio)
    bool process_order_fill(const OrderId& order_id, Quantity filled_qty,
                            Price fill_price);

    /// Нужна ли отмена остатка (CancelRemaining)?
    /// @return order_id для отмены, если нужно
    struct CancelRemainingAction {
        bool needed{false};
        OrderId exchange_order_id{OrderId("")};
        Symbol symbol{Symbol("")};
        OrderId order_id{OrderId("")};
        Side side{Side::Buy};
    };

    /// Check if a partial fill should trigger cancel-remaining policy.
    /// Called after process_fill_event on partial fills.
    CancelRemainingAction check_cancel_remaining(const OrderId& order_id) const;

    /// Attach an event journal for fee persistence (CRITICAL-3).
    /// Optional: if not set, fee events are not journaled.
    void set_journal(std::shared_ptr<persistence::EventJournal> journal) {
        journal_ = std::move(journal);
    }

private:
    /// Применить fill к портфелю (general — full order fill)
    void apply_fill_to_portfolio(const OrderRecord& order);

    /// Применить одиночный fill инкрементально (partial fill support)
    void apply_incremental_fill(const OrderRecord& order, const FillEvent& fill);

    /// Вычислить PnL для SELL/Close fill
    double compute_gross_pnl(const OrderRecord& order) const;

    /// Вычислить PnL для одного инкрементального fill delta
    double compute_incremental_pnl(const OrderRecord& order,
                                   Quantity fill_qty, Price fill_price) const;

    /// Append a FeeCharged event to the journal for crash-recovery durability.
    void journal_fee_event(const Symbol& symbol, const OrderId& order_id,
                           double fee, double fill_price, double fill_qty);

    OrderRegistry& registry_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<persistence::EventJournal> journal_;
    const ExecutionConfig& config_;
};

} // namespace tb::execution
